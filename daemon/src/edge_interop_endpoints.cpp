#include "edge_interop_endpoints.h"
#include "edge_interop_auth.h"
#include "config_endpoint.h"
#include "edge_confidentiality.h"
#include "edge_node_consensus.h"
#include "edge_runtime_endpoints.h"

#include "cbor_decode.h"
#include "cbor_encode.h"
#include "daemon_auth.h"
#include "edge_rules.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoints.h"

#include "base64.h"

#include "agent_sha256.h"
#include "agent/ed25519.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {

namespace {
static bool select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  const std::unordered_set<std::string>* exclude_node_ids_or_null,
  std::string* out_node_id
) {
  return edge_select_node_match_any(db, requires_tools, tags_all, tags_any, tags_none, exclude_node_ids_or_null, out_node_id);
}

static void append_edge_verify_chain_result_json(const EdgeVerifyChainResult& result, Json::Value* out) {
  if (!out) return;
  *out = Json::Value(Json::objectValue);
  (*out)["verified"] = result.verified;
  (*out)["verify_error_code"] = result.verify_error_code;
  (*out)["verify_error_depth"] = result.verify_error_depth;
  if (!result.verify_error.empty()) (*out)["verify_error"] = result.verify_error;
  if (!result.leaf_subject.empty()) (*out)["leaf_subject"] = result.leaf_subject;
  if (!result.leaf_issuer.empty()) (*out)["leaf_issuer"] = result.leaf_issuer;
  if (!result.leaf_sha256_hex.empty()) (*out)["leaf_sha256"] = result.leaf_sha256_hex;
  if (!result.chain_subjects.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.chain_subjects) if (!s.empty()) arr.append(s);
    (*out)["verified_chain_subjects"] = arr;
  }
  if (!result.matched_root_kids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.matched_root_kids) if (!s.empty()) arr.append(s);
    (*out)["matched_root_kids"] = arr;
  }
}

static bool extract_manifest_identity_cert_inputs(
  const Json::Value& manifest,
  std::string* out_cert_pem,
  std::vector<std::string>* out_chain_pem,
  bool* out_has_cert,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_cert_pem) out_cert_pem->clear();
  if (out_chain_pem) out_chain_pem->clear();
  if (out_has_cert) *out_has_cert = false;
  if (!manifest.isObject()) {
    if (out_error) *out_error = "manifest must be an object";
    return false;
  }
  if (!manifest.isMember("identity") || manifest["identity"].isNull()) return true;
  if (!manifest["identity"].isObject()) {
    if (out_error) *out_error = "manifest.identity must be an object";
    return false;
  }
  const Json::Value& identity = manifest["identity"];
  const bool has_cert_pem = identity.isMember("cert_pem") && !identity["cert_pem"].isNull();
  const bool has_chain = identity.isMember("cert_chain_pem") && !identity["cert_chain_pem"].isNull();
  if (!has_cert_pem && !has_chain) return true;
  if (!has_cert_pem || !identity["cert_pem"].isString() || trim_copy(identity["cert_pem"].asString()).empty()) {
    if (out_error) *out_error = "manifest.identity.cert_pem must be a non-empty string";
    return false;
  }
  if (out_cert_pem) *out_cert_pem = identity["cert_pem"].asString();
  if (has_chain) {
    if (!identity["cert_chain_pem"].isArray()) {
      if (out_error) *out_error = "manifest.identity.cert_chain_pem must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < identity["cert_chain_pem"].size(); i++) {
      if (!identity["cert_chain_pem"][i].isString()) {
        if (out_error) *out_error = "manifest.identity.cert_chain_pem entries must be strings";
        return false;
      }
      const std::string pem = identity["cert_chain_pem"][i].asString();
      if (!trim_copy(pem).empty() && out_chain_pem) out_chain_pem->push_back(pem);
    }
  }
  if (out_has_cert) *out_has_cert = true;
  return true;
}

static bool build_manifest_identity_cert_verify(
  const DaemonConfig& cfg,
  const Json::Value& manifest,
  Json::Value* out_verify,
  bool* out_has_cert,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_verify) *out_verify = Json::Value(Json::nullValue);
  std::string cert_pem;
  std::vector<std::string> chain_pem;
  bool has_cert = false;
  std::string xerr;
  if (!extract_manifest_identity_cert_inputs(manifest, &cert_pem, &chain_pem, &has_cert, &xerr)) {
    if (out_error) *out_error = xerr;
    return false;
  }
  if (out_has_cert) *out_has_cert = has_cert;
  if (!has_cert) return true;
  EdgeVerifyChainResult result;
  std::string verr;
  if (!verify_edge_cert_chain_against_roots(cfg, cert_pem, chain_pem, &result, &verr)) {
    if (out_error) *out_error = verr;
    return false;
  }
  if (out_verify) append_edge_verify_chain_result_json(result, out_verify);
  return true;
}

static Json::Value parse_health_json_best_effort(const std::string& health_json) {
  if (health_json.empty()) return Json::Value(Json::objectValue);
  Json::Value out;
  std::string err;
  if (!json_parse_any(health_json, &out, &err) || !out.isObject()) return Json::Value(Json::objectValue);
  return out;
}

static Json::Value build_edge_node_summary_json(
  const AgentDb::EdgeNodeRow* row_or_null,
  const Json::Value& runtime_or_null,
  const std::string& fallback_node_id
) {
  Json::Value row(Json::objectValue);
  const std::string runtime_node_id =
    runtime_or_null.isObject() && runtime_or_null.isMember("node_id") && runtime_or_null["node_id"].isString()
      ? trim_copy(runtime_or_null["node_id"].asString())
      : std::string();
  const std::string node_id = row_or_null ? row_or_null->node_id : (!runtime_node_id.empty() ? runtime_node_id : fallback_node_id);
  row["node_id"] = node_id;

  if (row_or_null) {
    if (!row_or_null->model.empty()) row["model"] = row_or_null->model;
    if (!row_or_null->fw_git_sha.empty()) row["fw_git_sha"] = row_or_null->fw_git_sha;
    if (!row_or_null->caps_sha256.empty()) row["caps_sha256"] = row_or_null->caps_sha256;
    row["last_hello_utc_ms"] = (Json::Int64)row_or_null->last_hello_utc_ms;
    row["last_heartbeat_utc_ms"] = (Json::Int64)row_or_null->last_heartbeat_utc_ms;
    if (!row_or_null->health_json.empty()) {
      Json::Value v;
      std::string perr2;
      if (json_parse_any(row_or_null->health_json, &v, &perr2) && v.isObject()) {
        row["health"] = v;
        if (v.isMember("consensus") && v["consensus"].isObject()) row["consensus"] = v["consensus"];
      }
    }
  } else {
    if (runtime_or_null.isObject() && runtime_or_null.isMember("model") && runtime_or_null["model"].isString()) {
      row["model"] = trim_copy(runtime_or_null["model"].asString());
    }
    if (runtime_or_null.isObject() && runtime_or_null.isMember("fw_git_sha") && runtime_or_null["fw_git_sha"].isString()) {
      row["fw_git_sha"] = trim_copy(runtime_or_null["fw_git_sha"].asString());
    }
    row["last_hello_utc_ms"] = (Json::Int64)0;
    row["last_heartbeat_utc_ms"] = (Json::Int64)0;
  }

  if (runtime_or_null.isObject()) row["consensus_runtime"] = runtime_or_null;
  return row;
}

static void append_runtime_only_edge_node_summaries(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  size_t limit,
  std::unordered_set<std::string>* seen_node_ids,
  Json::Value* arr
) {
  if (!arr || !arr->isArray() || !seen_node_ids || !db_or_null || !db_or_null->is_open()) return;
  if (arr->size() >= limit) return;
  const size_t remaining = limit - arr->size();
  const std::vector<std::string> runtime_node_ids = edge_consensus_runtime_node_ids(db_or_null, remaining + seen_node_ids->size());
  for (const auto& node_id : runtime_node_ids) {
    if (arr->size() >= limit) break;
    if (seen_node_ids->find(node_id) != seen_node_ids->end()) continue;
    Json::Value runtime = edge_consensus_runtime_status_json_for_node(cfg, db_or_null, node_id);
    if (!runtime.isObject()) continue;
    arr->append(build_edge_node_summary_json(nullptr, runtime, node_id));
    seen_node_ids->insert(node_id);
  }
}

static void append_edge_node_detail_json(
  const DaemonConfig& cfg,
  const AgentDb::EdgeNodeRow* row_or_null,
  Json::Value* out
) {
  if (!out || !out->isObject()) return;
  if (!row_or_null) {
    (*out)["has_manifest"] = false;
    return;
  }
  if (!row_or_null->tags_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->tags_json, &v, &perr2) && v.isArray()) (*out)["tags"] = v;
  }
  if (!row_or_null->tools_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->tools_json, &v, &perr2) && v.isArray()) (*out)["tools"] = v;
  }
  if (!row_or_null->hardware_presence_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->hardware_presence_json, &v, &perr2) && v.isObject()) (*out)["hardware_presence"] = v;
  }
  if (!row_or_null->manifest_json.empty()) {
    Json::Value manifest;
    std::string perr2;
    Json::Value verify(Json::nullValue);
    bool have_identity_cert = false;
    std::string verr;
    if (json_parse_any(row_or_null->manifest_json, &manifest, &perr2) && manifest.isObject() &&
        build_manifest_identity_cert_verify(cfg, manifest, &verify, &have_identity_cert, &verr) &&
        have_identity_cert && verify.isObject()) {
      (*out)["identity_cert_verify"] = verify;
    }
  }
  (*out)["has_manifest"] = !row_or_null->manifest_json.empty();
}

static bool collect_consensus_target_node_ids(
  const Json::Value& body,
  const std::string& env_to,
  std::vector<std::string>* out_node_ids,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_node_ids) return false;
  out_node_ids->clear();
  std::unordered_set<std::string> seen;
  auto push_node_id = [&](const std::string& node_id) -> bool {
    const std::string nid = trim_copy(node_id);
    if (nid.empty()) return true;
    if (!edge_id_is_safe(nid)) {
      if (out_error) *out_error = "invalid consensus target_node_id";
      return false;
    }
    if (seen.insert(nid).second) out_node_ids->push_back(nid);
    return true;
  };

  if (body.isMember("target_node_id") && !body["target_node_id"].isNull()) {
    if (!body["target_node_id"].isString()) {
      if (out_error) *out_error = "target_node_id must be string";
      return false;
    }
    if (!push_node_id(body["target_node_id"].asString())) return false;
  }
  if (body.isMember("target_node_ids") && !body["target_node_ids"].isNull()) {
    if (!body["target_node_ids"].isArray()) {
      if (out_error) *out_error = "target_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < body["target_node_ids"].size(); i++) {
      if (!body["target_node_ids"][i].isString()) {
        if (out_error) *out_error = "target_node_ids entries must be strings";
        return false;
      }
      if (!push_node_id(body["target_node_ids"][i].asString())) return false;
    }
  }
  if (out_node_ids->empty() && env_to.rfind("node:", 0) == 0 && env_to.size() > 5) {
    if (!push_node_id(env_to.substr(5))) return false;
  }
  return true;
}

static bool upsert_edge_node_consensus_health(
  AgentDb* db_or_null,
  const std::string& node_id,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& target_node_ids,
  const std::string& original_msg_id,
  int64_t now_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db_or_null || !db_or_null->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  AgentDb::EdgeNodeRow row;
  std::string err;
  if (!db_or_null->get_edge_node(node_id, &row, &err)) row.node_id = node_id;
  Json::Value health = parse_health_json_best_effort(row.health_json);
  Json::Value consensus(Json::objectValue);
  if (health.isMember("consensus") && health["consensus"].isObject()) consensus = health["consensus"];
  consensus["schema"] = "edge_node_consensus_status_v1";
  consensus["updated_utc_ms"] = (Json::Int64)now_utc_ms;
  if (!original_msg_id.empty()) consensus["last_msg_id"] = original_msg_id;
  consensus["last_frame_id"] = frame.frame_id;
  consensus["last_frame_kind"] = frame.kind;
  consensus["current_term"] = Json::UInt64(frame.term);
  if (!frame.decision_sha256.empty()) consensus["decision_sha256"] = frame.decision_sha256;
  if (!frame.candidate_node_id.empty()) consensus["candidate_node_id"] = frame.candidate_node_id;
  if (!frame.leader_node_id.empty()) consensus["leader_node_id"] = frame.leader_node_id;
  if (frame.kind == "vote_grant") consensus["granted"] = frame.granted;
  consensus["from"] = edge_consensus_identity_to_json(frame.from);
  if (!target_node_ids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    consensus["target_node_ids"] = arr;
    consensus["forwarded_count"] = (Json::UInt64)target_node_ids.size();
  } else {
    consensus["forwarded_count"] = (Json::UInt64)0;
    consensus.removeMember("target_node_ids");
  }
  if (!frame.vote_witnesses.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& witness : frame.vote_witnesses) {
      Json::Value w(Json::objectValue);
      w["node_id"] = witness.node_id;
      if (!witness.manifest_sha256.empty()) w["manifest_sha256"] = witness.manifest_sha256;
      w["trust_epochs"] = edge_consensus_epochs_to_json(witness.trust_epochs);
      arr.append(w);
    }
    consensus["vote_witnesses"] = arr;
    consensus["vote_witness_count"] = (Json::UInt64)frame.vote_witnesses.size();
  } else {
    consensus["vote_witness_count"] = (Json::UInt64)0;
    consensus.removeMember("vote_witnesses");
  }
  health["consensus"] = consensus;
  row.health_json = edge_json_stringify_compact(health);
  return db_or_null->upsert_edge_node(row, out_error);
}

static bool canonical_json_bytes(const Json::Value& v, std::string* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string raw = Json::writeString(wb, v);

  char* canon = nullptr;
  size_t canon_len = 0;
  char errbuf[128] = {0};
  const agent_status_t st =
    agent_json_c14n_canonicalize(raw.data(), raw.size(), &canon, &canon_len, errbuf, sizeof(errbuf));
  if (st != AGENT_OK || !canon) {
    if (out_err) *out_err = errbuf[0] ? std::string(errbuf) : "c14n_failed";
    if (canon) agent_free(canon);
    return false;
  }
  out->assign(canon, canon_len);
  agent_free(canon);
  return true;
}

static bool parse_hex_bytes(const std::string& hex, std::vector<uint8_t>* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  if (hex.size() % 2 != 0) {
    if (out_err) *out_err = "hex string must have even length";
    return false;
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      if (out_err) *out_err = "hex string contains non-hex characters";
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

static bool parse_seed_bytes(const std::string& text, std::vector<uint8_t>* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  const std::string s = trim_copy(text);
  if (s.empty()) {
    if (out_err) *out_err = "empty seed";
    return false;
  }
  bool hex_candidate = (s.size() == 64);
  if (hex_candidate) {
    for (char c : s) {
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!ok) {
        hex_candidate = false;
        break;
      }
    }
  }
  if (hex_candidate) {
    std::string herr;
    if (!parse_hex_bytes(s, out, &herr)) {
      if (out_err) *out_err = herr;
      return false;
    }
  } else {
    std::string raw;
    std::string berr;
    if (!base64_decode(s, &raw, &berr)) {
      if (out_err) *out_err = berr.empty() ? "invalid base64 seed" : berr;
      return false;
    }
    out->assign(raw.begin(), raw.end());
  }
  if (out->size() != 32) {
    if (out_err) *out_err = "seed must be 32 bytes";
    return false;
  }
  return true;
}

static bool compute_manifest_sha256(const Json::Value& manifest, std::string* out_sha256, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_sha256) return false;
  out_sha256->clear();
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string raw = Json::writeString(wb, manifest);
  char token[80] = {0};
  char errbuf[128] = {0};
  const agent_status_t st =
    agent_json_c14n_sha256_token(raw.data(), raw.size(), token, errbuf, sizeof(errbuf));
  if (st != AGENT_OK || token[0] == '\0') {
    if (out_err) *out_err = errbuf[0] ? std::string(errbuf) : "manifest_hash_failed";
    return false;
  }
  *out_sha256 = std::string(token);
  return true;
}

static bool maybe_sign_manifest_bundle(const DaemonConfig& cfg, Json::Value* bundle, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!bundle || !bundle->isObject()) {
    if (out_error) *out_error = "internal error";
    return false;
  }

  const bool hmac_kid_set = !cfg.run_attest_hmac_kid.empty();
  const bool hmac_key_set = !cfg.run_attest_hmac_key.empty();
  const bool ed_kid_set = !cfg.run_attest_ed25519_kid.empty();
  const bool ed_seed_set = !cfg.run_attest_ed25519_seed.empty();
  const int sign_modes = (hmac_kid_set || hmac_key_set ? 1 : 0) + (ed_kid_set || ed_seed_set ? 1 : 0);
  if (sign_modes == 0) return true;
  if (sign_modes > 1) {
    if (out_error) *out_error = "attest_sign_config_invalid: multiple signing modes configured";
    return false;
  }
  if ((hmac_kid_set || hmac_key_set) && (!hmac_kid_set || !hmac_key_set)) {
    if (out_error) *out_error = "attest_sign_config_invalid: missing AGENTD_RUN_ATTEST_HMAC_KID or AGENTD_RUN_ATTEST_HMAC_KEY";
    return false;
  }
  if ((ed_kid_set || ed_seed_set) && (!ed_kid_set || !ed_seed_set)) {
    if (out_error) *out_error = "attest_sign_config_invalid: missing AGENTD_RUN_ATTEST_ED25519_KID or AGENTD_RUN_ATTEST_ED25519_SEED";
    return false;
  }

  Json::Value signable = *bundle;
  signable.removeMember("attest");
  std::string canon;
  std::string cerr;
  if (!canonical_json_bytes(signable, &canon, &cerr)) {
    if (out_error) *out_error = std::string("attest_sign_failed: ") + (cerr.empty() ? "c14n_failed" : cerr);
    return false;
  }

  Json::Value sig(Json::objectValue);
  sig["schema"] = "edge_node_manifest_attest_v1";
  sig["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  sig["signing_schema"] = "edge_node_manifest_bundle_v1";
  if (bundle->isMember("node_id")) sig["node_id"] = (*bundle)["node_id"];
  if (bundle->isMember("caps_sha256")) sig["caps_sha256"] = (*bundle)["caps_sha256"];
  if (bundle->isMember("manifest_sha256")) sig["manifest_sha256"] = (*bundle)["manifest_sha256"];

  if (hmac_kid_set) {
    std::array<uint8_t, 32> mac{};
    agent_hmac_sha256(
      cfg.run_attest_hmac_key.data(),
      cfg.run_attest_hmac_key.size(),
      canon.data(),
      canon.size(),
      mac.data()
    );
    sig["alg"] = "hmac-sha256";
    sig["kid"] = cfg.run_attest_hmac_kid;
    sig["sig"] = base64_encode(reinterpret_cast<const char*>(mac.data()), mac.size());
  } else {
    std::vector<uint8_t> seed;
    std::string serr;
    if (!parse_seed_bytes(cfg.run_attest_ed25519_seed, &seed, &serr)) {
      if (out_error) *out_error = std::string("attest_sign_failed: ") + (serr.empty() ? "invalid_ed25519_seed" : serr);
      return false;
    }
    uint8_t pubkey[32] = {0};
    uint8_t sig_bytes[64] = {0};
    agent_ed25519_publickey(seed.data(), pubkey);
    agent_ed25519_sign(canon.data(), canon.size(), seed.data(), pubkey, sig_bytes);
    sig["alg"] = "ed25519";
    sig["kid"] = cfg.run_attest_ed25519_kid;
    sig["pubkey"] = base64_encode(reinterpret_cast<const char*>(pubkey), sizeof(pubkey));
    sig["sig"] = base64_encode(reinterpret_cast<const char*>(sig_bytes), sizeof(sig_bytes));
  }

  (*bundle)["attest"] = sig;
  return true;
}

static bool build_edge_node_manifest_bundle(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const std::string& node_id,
  Json::Value* out_bundle,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bundle) return false;
  if (!db_or_null || !db_or_null->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    if (out_error) *out_error = "missing/invalid node_id";
    return false;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(node_id, &n, &err)) {
    if (out_error) *out_error = "node not found";
    return false;
  }
  if (n.manifest_json.empty()) {
    if (out_error) *out_error = "node has no manifest";
    return false;
  }

  Json::Value manifest;
  std::string perr;
  if (!json_parse_any(n.manifest_json, &manifest, &perr) || !manifest.isObject()) {
    if (out_error) *out_error = "failed to parse stored manifest";
    return false;
  }

  std::string manifest_sha256;
  std::string sha_err;
  if (!compute_manifest_sha256(manifest, &manifest_sha256, &sha_err)) {
    if (out_error) *out_error = "failed to hash manifest";
    return false;
  }

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "edge_node_manifest_bundle_v1";
  bundle["created_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  bundle["node_id"] = n.node_id;
  if (!n.caps_sha256.empty()) bundle["caps_sha256"] = n.caps_sha256;
  bundle["manifest_sha256"] = manifest_sha256;
  bundle["manifest_sha256_alg"] = "agent_json_c14n_v1";
  if (!n.model.empty()) bundle["model"] = n.model;
  if (!n.fw_git_sha.empty()) bundle["fw_git_sha"] = n.fw_git_sha;
  bundle["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
  bundle["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
  bundle["manifest"] = manifest;

  if (manifest.isMember("tools") && manifest["tools"].isArray()) {
    bundle["tools"] = manifest["tools"];
  } else if (!n.tools_json.empty()) {
    Json::Value v;
    std::string derr;
    if (json_parse_any(n.tools_json, &v, &derr) && v.isArray()) bundle["tools"] = v;
  }

  bool have_tags = false;
  if (!n.tags_json.empty()) {
    Json::Value v;
    std::string derr;
    if (json_parse_any(n.tags_json, &v, &derr) && v.isArray()) {
      bundle["tags"] = v;
      have_tags = true;
    }
  }
  if (!have_tags && manifest.isMember("tags") && manifest["tags"].isArray()) bundle["tags"] = manifest["tags"];

  bool have_presence = false;
  if (!n.hardware_presence_json.empty()) {
    Json::Value v;
    std::string derr;
    if (json_parse_any(n.hardware_presence_json, &v, &derr) && v.isObject()) {
      bundle["hardware_presence"] = v;
      have_presence = true;
    }
  }
  if (!have_presence && manifest.isMember("hardware") && manifest["hardware"].isObject() &&
      manifest["hardware"].isMember("presence") && manifest["hardware"]["presence"].isObject()) {
    bundle["hardware_presence"] = manifest["hardware"]["presence"];
  }
  {
    Json::Value verify(Json::nullValue);
    bool have_identity_cert = false;
    std::string verr;
    if (build_manifest_identity_cert_verify(cfg, manifest, &verify, &have_identity_cert, &verr) && have_identity_cert &&
        verify.isObject()) {
      bundle["identity_cert_verify"] = verify;
    }
  }

  std::string sign_err;
  if (!maybe_sign_manifest_bundle(cfg, &bundle, &sign_err)) {
    if (out_error) {
      *out_error = sign_err.empty() ? "manifest_bundle_sign_failed" : sign_err;
    }
    return false;
  }

  *out_bundle = bundle;
  return true;
}

}  // namespace

void handle_edge_message_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value env;
  std::string perr;
  {
    const std::string ct =
      req.headers.count("content-type") ? trim_copy(req.headers.at("content-type")) : "";
    const bool is_cbor = !ct.empty() && url_contains_ci(ct, "application/cbor");
    if (is_cbor) {
      if (!cbor_decode_to_json_value(req.body, &env, &perr) || !env.isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = std::string("invalid CBOR: ") + (perr.empty() ? "parse failed" : perr);
        resp->body = edge_json_stringify_compact(o);
        return;
      }
    } else {
      if (!json_parse_object(req.body, &env, &perr)) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = std::string("invalid JSON: ") + perr;
        resp->body = edge_json_stringify_compact(o);
        return;
      }
    }
  }

  const std::string msg_id = env.isMember("msg_id") && env["msg_id"].isString() ? trim_copy(env["msg_id"].asString()) : "";
  const int64_t ts_utc_ms = env.isMember("ts_utc_ms") && env["ts_utc_ms"].isInt64() ? env["ts_utc_ms"].asInt64()
    : (env.isMember("ts_utc_ms") && env["ts_utc_ms"].isUInt64() ? (int64_t)env["ts_utc_ms"].asUInt64() : 0);
  const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
  const std::string from_id = env.isMember("from") && env["from"].isString() ? trim_copy(env["from"].asString()) : "";
  std::string to_id;
  if (env.isMember("to")) {
    if (env["to"].isString()) to_id = trim_copy(env["to"].asString());
    else if (env["to"].isNull()) to_id.clear();
    else {
      resp->status = 400;
      resp->body = json_error_body("invalid envelope.to (expected string|null)");
      return;
    }
  }
  Json::Value body(Json::nullValue);
  std::string ccode;
  std::string cerr;
  if (!edge_confidentiality_extract_envelope_body(
        env,
        cfg.edge_confidentiality_keys,
        cfg.edge_confidentiality_required,
        &body,
        nullptr,
        nullptr,
        &ccode,
        &cerr)) {
    if (ccode == "confidentiality_required" || ccode == "unknown_confidential_kid" || ccode == "decrypt_failed") {
      resp->status = 401;
    } else if (ccode == "internal") {
      resp->status = 500;
    } else {
      resp->status = 400;
    }
    resp->body = json_error_body(cerr.empty() ? "invalid envelope body" : cerr);
    return;
  }
  const Json::Value trace = env.isMember("trace") && env["trace"].isObject() ? env["trace"] : Json::Value(Json::nullValue);

  if (msg_id.empty() || type.empty() || !body.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("invalid envelope (missing msg_id/type/body_or_body_enc)");
    return;
  }
  if (!edge_id_is_safe(type) || msg_id.size() > 128) {
    resp->status = 400;
    resp->body = json_error_body("invalid envelope msg_id/type");
    return;
  }

  const int64_t now = edge_unix_ms_now();
  if (!verify_edge_envelope_auth_best_effort(cfg, env, now, ts_utc_ms, from_id, body, resp)) return;

  // Persist inbound (dedupe by msg_id).
  bool deduped = false;
  {
    AgentDb::EdgeInboxMessageRow ir;
    ir.msg_id = msg_id;
    ir.ts_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : edge_unix_ms_now();
    ir.type = type;
    ir.from_id = from_id;
    ir.to_id = to_id;
    ir.envelope_json = edge_json_stringify_compact(env);
    std::string err;
    AgentDb::EdgeInboxAuthSeqGuard guard;
    AgentDb::EdgeInboxAuthSeqGuard* guard_or_null = nullptr;
    if (cfg.edge_auth_require_seq && env.isMember("auth") && env["auth"].isObject()) {
      if (from_id.rfind("node:", 0) != 0 || from_id.size() <= 5) {
        resp->status = 401;
        resp->body = json_error_body("edge auth seq requires envelope.from node:<node_id>");
        return;
      }
      const std::string node_id = trim_copy(from_id.substr(5));
      if (node_id.empty() || !edge_id_is_safe(node_id)) {
        resp->status = 401;
        resp->body = json_error_body("edge auth seq requires valid node_id");
        return;
      }
      const Json::Value& auth = env["auth"];
      int64_t seq = -1;
      if (!auth.isMember("seq")) {
        resp->status = 401;
        resp->body = json_error_body("missing envelope.auth.seq");
        return;
      }
      if (auth["seq"].isInt64()) seq = auth["seq"].asInt64();
      else if (auth["seq"].isUInt64()) {
        const auto u = auth["seq"].asUInt64();
        if (u <= (Json::UInt64)std::numeric_limits<int64_t>::max()) seq = (int64_t)u;
        else seq = -1;
      }
      if (seq < 0) {
        resp->status = 401;
        resp->body = json_error_body("invalid envelope.auth.seq");
        return;
      }
      guard.node_id = node_id;
      guard.seq = seq;
      guard_or_null = &guard;
    }

    bool seq_rejected = false;
    if (!db_or_null->insert_edge_inbox_message_with_seq_guard(ir, guard_or_null, &deduped, &seq_rejected, &err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist edge inbox message";
      o["detail"] = err;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    if (seq_rejected) {
      resp->status = 401;
      resp->body = json_error_body("edge auth seq replay");
      return;
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    if (deduped) o["deduped"] = true;
    resp->body = edge_json_stringify_compact(o);
  }

  if (deduped) {
    bool processed = false;
    std::string perr2;
    const bool found = db_or_null->get_edge_inbox_message_processed(msg_id, &processed, &perr2);
    if (found && processed) {
      // Already processed: keep the deduped response and return without re-applying side effects.
      Json::Value o(Json::objectValue);
      o["ok"] = true;
      o["deduped"] = true;
      o["processed"] = true;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
  }

  struct InboxProcessedGuard {
    AgentDb* db = nullptr;
    std::string msg_id;
    int64_t now = 0;
    HttpResponse* resp = nullptr;
    bool armed = false;
    ~InboxProcessedGuard() {
      if (!armed || !db || msg_id.empty() || !resp) return;
      // Mark as processed for any non-5xx response. 5xx errors are considered retryable.
      if (resp->status >= 500) return;
      std::string ign;
      (void)db->mark_edge_inbox_message_processed(msg_id, now, &ign);
    }
  };

  InboxProcessedGuard inbox_guard;
  inbox_guard.db = db_or_null;
  inbox_guard.msg_id = msg_id;
  inbox_guard.now = now;
  inbox_guard.resp = resp;
  inbox_guard.armed = true;

  auto sanitize_id_token = [](std::string s, size_t max_len) -> std::string {
    if (s.size() > max_len) s.resize(max_len);
    if (s.empty()) return s;
    for (char& c : s) {
      const bool ok =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == ':';
      if (!ok) c = '_';
    }
    // Avoid leading/trailing underscores from weird msg_ids; best-effort.
    while (!s.empty() && s.front() == '_') s.erase(s.begin());
    while (!s.empty() && s.back() == '_') s.pop_back();
    if (s.empty()) s = "msg";
    if (s.size() > max_len) s.resize(max_len);
    return s;
  };

  auto try_send_outbox_ack = [&](const std::string& ack_type, const Json::Value& ack_body) {
    std::string reply_node_id;
    if (from_id.rfind("node:", 0) == 0) reply_node_id = from_id.substr(5);
    if (body.isMember("node_id") && body["node_id"].isString()) reply_node_id = trim_copy(body["node_id"].asString());
    if (reply_node_id.empty() || !edge_id_is_safe(reply_node_id)) return;
    Json::Value ack(Json::objectValue);
    ack["msg_id"] = edge_make_uuidish_msg_id();
    ack["ts_utc_ms"] = (Json::Int64)now;
    ack["type"] = ack_type;
    ack["from"] = "platform";
    ack["to"] = edge_node_to_prefix(reply_node_id);
    ack["body"] = ack_body.isObject() ? ack_body : Json::Value(Json::objectValue);
    AgentDb::EdgeOutboxMessageRow orow;
    orow.node_id = reply_node_id;
    orow.ts_utc_ms = now;
    orow.envelope_json = edge_json_stringify_compact(ack);
    (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
  };

  // Durable workflow handoff over UM‑BMP ingress:
  // - allows resource-constrained nodes (MCU agent_core) to hand off durable orchestration to the platform
  // - transport-agnostic; works via MQTT/LoRa gateways mapped to /api/v1/edge/message
  if (type == "DURABLE_WORKFLOW_SUBMIT") {
    Json::Value wfargs = body;
    if (body.isMember("workflow") && body["workflow"].isObject()) wfargs = body["workflow"];
    if (!wfargs.isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid DURABLE_WORKFLOW_SUBMIT body (expected object)");
      return;
    }

    // Canonicalize workflow_id/idempotency_key using msg_id (retry-safe at transport level).
    const std::string token = sanitize_id_token(msg_id, 96);
    std::string wid =
      wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
    if (wid.empty()) wid = std::string("wf:") + token;
    wfargs["workflow_id"] = sanitize_id_token(wid, 128);

    if (!wfargs.isMember("trace_id") || !wfargs["trace_id"].isString() || wfargs["trace_id"].asString().empty()) {
      wfargs["trace_id"] = wfargs["workflow_id"];
    }

    if (!wfargs.isMember("idempotency_key") || !wfargs["idempotency_key"].isString() || wfargs["idempotency_key"].asString().empty()) {
      // Prefer workflow-scoped idempotency when workflow_id is caller-provided, so callers can safely retry even if
      // a transport bridge regenerates msg_id. If workflow_id is derived from msg_id, this still de-dupes within that
      // message id.
      const std::string wf_token =
        wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
      const std::string ik = !wf_token.empty()
        ? (std::string("edge_wf:") + wf_token)
        : (std::string("edge_msg:") + token);
      wfargs["idempotency_key"] = sanitize_id_token(ik, 128);
    }

    // Safety: nodes should not send inline API keys by default.
    wfargs["allow_inline_api_keys"] = false;

    HttpRequest req2 = req;
    req2.body = json_stringify(wfargs);
    HttpResponse r2;
    handle_workflow_submit_endpoint(cfg, cors_cfg, db_or_null, req2, &r2);
    *resp = r2;

    // Best-effort: notify the node via outbox.
    Json::Value ack_body(Json::objectValue);
    ack_body["ok"] = (resp->status >= 200 && resp->status < 300);
    ack_body["workflow_id"] = wfargs["workflow_id"];
    ack_body["op"] = "submit";
    try_send_outbox_ack("DURABLE_WORKFLOW_ACK", ack_body);
    return;
  }

  if (type == "DURABLE_WORKFLOW_CANCEL") {
    const std::string workflow_id = body.isMember("workflow_id") && body["workflow_id"].isString() ? trim_copy(body["workflow_id"].asString()) : "";
    if (workflow_id.empty()) {
      resp->status = 400;
      resp->body = json_error_body("missing workflow_id");
      return;
    }
    Json::Value args(Json::objectValue);
    args["workflow_id"] = workflow_id;
    HttpRequest req2 = req;
    req2.body = json_stringify(args);
    HttpResponse r2;
    handle_workflow_cancel_endpoint(cfg, cors_cfg, db_or_null, req2, &r2);
    *resp = r2;

    Json::Value ack_body(Json::objectValue);
    ack_body["ok"] = (resp->status >= 200 && resp->status < 300);
    ack_body["workflow_id"] = workflow_id;
    ack_body["op"] = "cancel";
    try_send_outbox_ack("DURABLE_WORKFLOW_ACK", ack_body);
    return;
  }

  if (type == "NODE_HELLO" || type == "NODE_HEARTBEAT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string model = body.isMember("model") && body["model"].isString() ? body["model"].asString() : "";
    const std::string fw = body.isMember("fw_git_sha") && body["fw_git_sha"].isString() ? body["fw_git_sha"].asString() : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    if (node_id.empty() || !edge_id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid node_id");
      return;
    }
    if (!caps_sha.empty() && !edge_sha256_token_is_safe(caps_sha)) {
      resp->status = 400;
      resp->body = json_error_body("invalid caps_sha256");
      return;
    }

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.model = model;
    nr.fw_git_sha = fw;
    nr.caps_sha256 = caps_sha;
    if (type == "NODE_HELLO") nr.last_hello_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;

    if (type == "NODE_HEARTBEAT") {
      const Json::Value health = body.isMember("health") ? body["health"] : Json::Value(Json::nullValue);
      const bool has_battery_pct = body.isMember("battery_pct") && (body["battery_pct"].isDouble() || body["battery_pct"].isInt());
      const bool has_rssi = body.isMember("rssi") && (body["rssi"].isDouble() || body["rssi"].isInt());

      Json::Value h(Json::objectValue);
      bool has_any = false;
      if (health.isObject()) {
        h = health;
        has_any = true;
      }
      if (has_battery_pct) {
        h["battery_pct"] = body["battery_pct"].asDouble();
        has_any = true;
      }
      if (has_rssi) {
        h["rssi"] = body["rssi"].asDouble();
        has_any = true;
      }
      if (has_any) nr.health_json = edge_json_stringify_compact(h);
    }

    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node record";
      o["detail"] = uerr;
      resp->body = edge_json_stringify_compact(o);
      return;
    }

    // Best-effort: if caps sha is unknown or changed, request full manifest.
    bool need_caps = false;
    if (!caps_sha.empty()) {
      AgentDb::EdgeNodeRow existing;
      std::string err;
      if (db_or_null->get_edge_node(node_id, &existing, &err)) {
        if (existing.caps_sha256 != caps_sha || existing.manifest_json.empty()) {
          need_caps = true;
        }
      } else {
        need_caps = true;
      }
    }
    if (need_caps) {
      Json::Value env(Json::objectValue);
      env["msg_id"] = edge_make_uuidish_msg_id();
      env["ts_utc_ms"] = (Json::Int64)now;
      env["type"] = "PLATFORM_CAPS_REQ";
      env["from"] = "platform";
      env["to"] = edge_node_to_prefix(node_id);
      Json::Value b(Json::objectValue);
      b["node_id"] = node_id;
      b["want"] = "full";
      env["body"] = b;

      AgentDb::EdgeOutboxMessageRow orow;
      orow.node_id = node_id;
      orow.ts_utc_ms = now;
      orow.envelope_json = edge_json_stringify_compact(env);
      int64_t outbox_id = 0;
      std::string err;
      (void)db_or_null->insert_edge_outbox_message(orow, &outbox_id, &err);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_CAPS_RSP") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const Json::Value manifest = body.isMember("manifest") ? body["manifest"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !edge_id_is_safe(node_id) || !manifest.isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid NODE_CAPS_RSP body");
      return;
    }
    Json::Value identity_cert_verify(Json::nullValue);
    bool have_identity_cert = false;
    std::string cert_verr;
    if (!build_manifest_identity_cert_verify(cfg, manifest, &identity_cert_verify, &have_identity_cert, &cert_verr)) {
      const bool internal_err =
        cert_verr.rfind("internal:", 0) == 0 ||
        cert_verr.rfind("invalid configured cert root", 0) == 0;
      resp->status = internal_err ? 500 : 400;
      resp->body = json_error_body(cert_verr.empty() ? "invalid manifest.identity certificate material" : cert_verr);
      return;
    }
    if (cfg.edge_auth_require_manifest_cert_chain) {
      if (!have_identity_cert) {
        resp->status = 400;
        resp->body = json_error_body("manifest.identity.cert_pem required when edge_auth_require_manifest_cert_chain=true");
        return;
      }
      if (!identity_cert_verify.isObject() || !identity_cert_verify.isMember("verified") ||
          !identity_cert_verify["verified"].asBool()) {
        const std::string verr =
          identity_cert_verify.isObject() && identity_cert_verify.isMember("verify_error") &&
              identity_cert_verify["verify_error"].isString()
            ? trim_copy(identity_cert_verify["verify_error"].asString())
            : std::string("manifest identity cert chain did not verify");
        resp->status = 400;
        resp->body = json_error_body(verr);
        return;
      }
    }

    std::string tags_json, tools_json, hw_json;
    edge_manifest_extract_best_effort(manifest, &tags_json, &tools_json, &hw_json);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    if (manifest.isMember("caps_sha256") && manifest["caps_sha256"].isString()) {
      const std::string caps_sha = trim_copy(manifest["caps_sha256"].asString());
      if (!caps_sha.empty() && !edge_sha256_token_is_safe(caps_sha)) {
        resp->status = 400;
        resp->body = json_error_body("invalid manifest.caps_sha256");
        return;
      }
      nr.caps_sha256 = caps_sha;
    }
    nr.manifest_json = edge_json_stringify_compact(manifest);
    nr.tags_json = tags_json;
    nr.tools_json = tools_json;
    nr.hardware_presence_json = hw_json;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node manifest";
      o["detail"] = uerr;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "CONSENSUS_FRAME") {
    if (!body.isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid CONSENSUS_FRAME body");
      return;
    }
    if (from_id.rfind("node:", 0) != 0 || from_id.size() <= 5) {
      resp->status = 400;
      resp->body = json_error_body("CONSENSUS_FRAME requires envelope.from node:<node_id>");
      return;
    }
    const std::string source_node_id = trim_copy(from_id.substr(5));
    if (source_node_id.empty() || !edge_id_is_safe(source_node_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid consensus source node_id");
      return;
    }
    if (!body.isMember("frame") || !body["frame"].isObject()) {
      resp->status = 400;
      resp->body = json_error_body("CONSENSUS_FRAME requires body.frame object");
      return;
    }

    EdgeConsensusFrame frame;
    std::string ferr;
    if (!edge_consensus_frame_from_json(body["frame"], &frame, &ferr)) {
      resp->status = 400;
      resp->body = json_error_body(ferr.empty() ? "invalid consensus frame" : ferr);
      return;
    }
    if (frame.from.node_id != source_node_id) {
      resp->status = 400;
      resp->body = json_error_body("consensus frame.from.node_id must match envelope.from");
      return;
    }

    std::vector<std::string> target_node_ids;
    std::string terr;
    if (!collect_consensus_target_node_ids(body, to_id, &target_node_ids, &terr)) {
      resp->status = 400;
      resp->body = json_error_body(terr.empty() ? "invalid consensus targets" : terr);
      return;
    }

    std::string herr;
    if (!upsert_edge_node_consensus_health(db_or_null, source_node_id, frame, target_node_ids, msg_id, now, &herr)) {
      resp->status = 500;
      resp->body = json_error_body(herr.empty() ? "failed to persist consensus status" : herr);
      return;
    }

    int64_t last_outbox_id = 0;
    const Json::Value frame_json = edge_consensus_frame_to_json(frame);
    for (const auto& target_node_id : target_node_ids) {
      Json::Value relay_env(Json::objectValue);
      relay_env["msg_id"] = edge_make_uuidish_msg_id();
      relay_env["ts_utc_ms"] = (Json::Int64)now;
      relay_env["type"] = "CONSENSUS_FRAME";
      relay_env["from"] = "platform";
      relay_env["to"] = edge_node_to_prefix(target_node_id);
      Json::Value relay_body(Json::objectValue);
      relay_body["relay_from"] = source_node_id;
      relay_body["original_msg_id"] = msg_id;
      relay_body["frame"] = frame_json;
      relay_env["body"] = relay_body;
      AgentDb::EdgeOutboxMessageRow orow;
      orow.node_id = target_node_id;
      orow.ts_utc_ms = now;
      orow.envelope_json = edge_json_stringify_compact(relay_env);
      std::string oerr;
      if (!db_or_null->insert_edge_outbox_message(orow, &last_outbox_id, &oerr)) {
        resp->status = 500;
        resp->body = json_error_body(oerr.empty() ? "failed to enqueue consensus relay" : oerr);
        return;
      }
    }

    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["source_node_id"] = source_node_id;
    o["forwarded_count"] = (Json::UInt64)target_node_ids.size();
    if (last_outbox_id > 0) o["last_outbox_id"] = (Json::Int64)last_outbox_id;
    o["frame"] = frame_json;
    Json::Value arr(Json::arrayValue);
    for (const auto& target_node_id : target_node_ids) arr.append(target_node_id);
    o["target_node_ids"] = arr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  auto update_task_state = [&](const std::string& task_id, const std::string& step_id, const std::string& state,
                               const std::string& result_json, const std::string& error_text, const Json::Value& event_data) {
    if (task_id.empty() || step_id.empty() || state.empty()) return;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (!db_or_null->get_edge_task(task_id, step_id, &tr, &terr)) {
      // Unknown task; ignore to keep ingestion robust.
      return;
    }
    const bool terminal = edge_is_terminal_task_state(tr.state);
    const int64_t ts_eff = ts_utc_ms > 0 ? ts_utc_ms : now;

    std::string got_idem;
    if (event_data.isObject() && event_data.isMember("idempotency_key") && event_data["idempotency_key"].isString()) {
      got_idem = trim_copy(event_data["idempotency_key"].asString());
    }
    const bool idempotency_mismatch =
      (!got_idem.empty() && !tr.idempotency_key.empty() && got_idem != tr.idempotency_key);

    std::string msg_trace_id;
    bool msg_trace_id_valid = true;
    if (event_data.isObject() && event_data.isMember("trace") && event_data["trace"].isObject() && event_data["trace"].isMember("trace_id") &&
        event_data["trace"]["trace_id"].isString()) {
      msg_trace_id = trim_copy(event_data["trace"]["trace_id"].asString());
      if (!msg_trace_id.empty() && !edge_trace_id_is_safe(msg_trace_id)) msg_trace_id_valid = false;
    }
    const std::string msg_trace_id_eff = msg_trace_id_valid ? msg_trace_id : "";
    // Allow trace_id backfill even on idempotency mismatch when the stored trace_id is empty
    // or still set to the workflow/task id (default edge workflow behavior). This preserves
    // trace correlation for edge workflows without mutating task state.
    const bool can_backfill_trace_id =
      !msg_trace_id_eff.empty() && (tr.trace_id.empty() || tr.trace_id == tr.task_id);

    // Do not regress/override terminal states set by the platform (e.g. deadline sweeper) or earlier completion.
    // We still persist the event for observability.
    const bool apply_update = !terminal && !idempotency_mismatch;
    std::string state_eff = state;
    std::string error_text_eff = error_text;
    bool attest_result_sha_mismatch = false;
    std::string attest_result_sha;
    std::string platform_hash_alg;
    std::string platform_c14n_error;
    bool attest_present = false;
    bool attest_sig_checked = false;
    bool attest_sig_ok = false;
    std::string attest_sig_error;
    if (apply_update) {
      if (state == "SUCCEEDED" && !result_json.empty()) {
        std::string stored_result_json = result_json;
        // Default signing/hash surface is the stored result JSON bytes.
        // We may override this below when the node supplies a separate `result.attest` blob.
        std::string hash_bytes = stored_result_json;
        platform_hash_alg = "raw_result_json_bytes_v0";

        // If the node included `result.attest`, persist it separately and exclude it from the stored result hash surface.
        //
        // Rationale:
        // - `attest.result_sha256` is intended to describe the work product (body.result without attest),
        //   not the attestation metadata itself.
        // - Including `attest` makes `result_sha256` self-referential once nodes include `result_sha256` and `sig`.
        if (event_data.isObject() && event_data.isMember("result") && event_data["result"].isObject()) {
          const Json::Value result = event_data["result"];
          if (result.isMember("attest") && result["attest"].isObject()) {
            Json::Value result2 = result;
            result2.removeMember("attest");
            stored_result_json = edge_json_stringify_compact(result2);
            hash_bytes = stored_result_json;
          }
        }

        // Best-effort: canonicalize to a portable form so heterogeneous nodes can match
        // `result_sha256` (attestation/quorum). If canonicalization fails, fall back to
        // hashing the platform-stored bytes.
        if (hash_bytes.size() <= 256 * 1024) {
          char* c14n = nullptr;
          size_t c14n_len = 0;
          char errbuf[256] = {0};
          const agent_status_t st =
            agent_json_c14n_canonicalize(hash_bytes.data(), hash_bytes.size(), &c14n, &c14n_len, errbuf, sizeof(errbuf));
          if (st == AGENT_OK && c14n && c14n_len > 0) {
            hash_bytes.assign(c14n, c14n_len);
            stored_result_json.assign(c14n, c14n_len);
            agent_free(c14n);
            platform_hash_alg = "agent_json_c14n_v1";
          } else {
            if (c14n) agent_free(c14n);
            platform_c14n_error = errbuf[0] ? std::string(errbuf) : "c14n_failed";
          }
        } else {
          platform_c14n_error = "result_json_too_large_for_c14n";
        }
        char hex[65] = {0};
        agent_sha256_hex_of_bytes(hash_bytes.data(), hash_bytes.size(), hex);
        tr.result_sha256 = std::string("sha256:") + hex;
        tr.result_json = stored_result_json;
      }
      if (state == "SUCCEEDED" && event_data.isObject() && event_data.isMember("result") && event_data["result"].isObject()) {
        const Json::Value result = event_data["result"];
        if (result.isMember("attest") && result["attest"].isObject()) {
          attest_present = true;
          std::string aj = edge_json_stringify_compact(result["attest"]);
          if (aj.size() > 8192) aj.resize(8192);
          tr.attest_json = aj;

          const Json::Value at = result["attest"];
          if (at.isMember("result_sha256") && at["result_sha256"].isString()) {
            attest_result_sha = trim_copy(at["result_sha256"].asString());
            if (!attest_result_sha.empty() && edge_sha256_token_is_safe(attest_result_sha) && !tr.result_sha256.empty() &&
                attest_result_sha != tr.result_sha256) {
              attest_result_sha_mismatch = true;
            }
          }
        }
      }
    }
    Json::Value d = event_data.isNull() ? Json::Value(Json::objectValue) : event_data;
    // Verify node-provided attestation signature when possible.
    //
    // When `edge_attest_required` / `edge_attest_require_sig` are enabled, we may fail the task on
    // missing/invalid attestation. Otherwise, we only emit evidence for debugging/correlation.
    if (d.isObject() && d.isMember("result") && d["result"].isObject() && d["result"].isMember("attest") && d["result"]["attest"].isObject()) {
      const Json::Value at = d["result"]["attest"];
      const std::string kid = at.isMember("kid") && at["kid"].isString() ? trim_copy(at["kid"].asString()) : "";
      const std::string alg = at.isMember("alg") && at["alg"].isString() ? trim_copy(at["alg"].asString()) : "";
      const std::string sig_b64 = at.isMember("sig") && at["sig"].isString() ? trim_copy(at["sig"].asString()) : "";
      const int64_t ats = at.isMember("ts_utc_ms") && at["ts_utc_ms"].isInt64() ? at["ts_utc_ms"].asInt64()
        : (at.isMember("ts_utc_ms") && at["ts_utc_ms"].isUInt64() ? (int64_t)at["ts_utc_ms"].asUInt64() : 0);
      const std::string rsha = at.isMember("result_sha256") && at["result_sha256"].isString() ? trim_copy(at["result_sha256"].asString()) : "";

      const bool can_try =
        !kid.empty() && !alg.empty() && !sig_b64.empty() && ats > 0 &&
        !task_id.empty() && !step_id.empty() && !got_idem.empty() &&
        !rsha.empty() && edge_sha256_token_is_safe(rsha) &&
        kid.size() <= 64 && edge_id_is_safe(kid);

      if (can_try) {
        attest_sig_checked = true;
        d["_attest_sig_checked"] = true;
        d["_attest_sig_kid"] = kid;
        d["_attest_sig_alg"] = alg;

        // Optional per-node key selection policy (same semantics as envelope auth).
        if (from_id.rfind("node:", 0) == 0 && from_id.size() > 5) {
          const std::string node_id_from = trim_copy(from_id.substr(5));
          if (!node_id_from.empty() && edge_id_is_safe(node_id_from)) {
            const std::string pol = cfg.edge_auth_kid_policy.empty() ? "any" : cfg.edge_auth_kid_policy;
            if (pol == "match_node") {
              if (kid != node_id_from) {
                d["_attest_sig_ok"] = false;
                d["_attest_sig_error"] = "kid_policy_violation";
              }
            } else if (pol == "node_prefix") {
              const std::string pref = node_id_from + ":";
              const bool ok = (kid == node_id_from) || (kid.size() > pref.size() && kid.rfind(pref, 0) == 0);
              if (!ok) {
                d["_attest_sig_ok"] = false;
                d["_attest_sig_error"] = "kid_policy_violation";
              }
            }
          }
        }

        if (!d.isMember("_attest_sig_ok")) {
          std::string node_id_from;
          if (from_id.rfind("node:", 0) == 0 && from_id.size() > 5) {
            node_id_from = trim_copy(from_id.substr(5));
          }
          std::string revoke_reason;
          if (edge_auth_is_revoked(cfg, node_id_from, kid, &revoke_reason)) {
            d["_attest_sig_ok"] = false;
            d["_attest_sig_error"] = revoke_reason;
          }
        }

        if (!d.isMember("_attest_sig_ok")) {
          const std::string alg_l = lower_copy(alg);
          const std::string input = umbmp_result_attest_input_v0_1(task_id, step_id, got_idem, rsha, ats);

          if (alg_l == "ed25519") {
            const auto it = cfg.edge_auth_ed25519_pubkeys.find(kid);
            if (it == cfg.edge_auth_ed25519_pubkeys.end() || it->second.empty()) {
              d["_attest_sig_ok"] = false;
              d["_attest_sig_error"] = "unknown_kid";
            } else {
              std::string pk_bytes;
              std::string perr;
              if (!base64_decode(it->second, &pk_bytes, &perr) || pk_bytes.size() != 32) {
                d["_attest_sig_ok"] = false;
                d["_attest_sig_error"] = "invalid_config_pubkey";
              } else {
                std::string sig_bytes;
                std::string serr;
                if (!base64_decode(sig_b64, &sig_bytes, &serr) || sig_bytes.size() != 64) {
                  d["_attest_sig_ok"] = false;
                  d["_attest_sig_error"] = "invalid_sig_b64";
                } else if (agent_ed25519_verify(input.data(), input.size(),
                           (const uint8_t*)pk_bytes.data(), (const uint8_t*)sig_bytes.data())) {
                  d["_attest_sig_ok"] = true;
                } else {
                  d["_attest_sig_ok"] = false;
                  d["_attest_sig_error"] = "verify_failed";
                }
              }
            }
          } else if (alg_l == "hmac-sha256") {
            const auto it = cfg.edge_auth_hmac_keys.find(kid);
            if (it == cfg.edge_auth_hmac_keys.end() || it->second.empty()) {
              d["_attest_sig_ok"] = false;
              d["_attest_sig_error"] = "unknown_kid";
            } else {
              std::string sig_bytes;
              std::string serr;
              if (!base64_decode(sig_b64, &sig_bytes, &serr) || sig_bytes.size() != 32) {
                d["_attest_sig_ok"] = false;
                d["_attest_sig_error"] = "invalid_sig_b64";
              } else {
                uint8_t mac[32];
                agent_hmac_sha256(
                  it->second.data(),
                  it->second.size(),
                  input.data(),
                  input.size(),
                  mac
                );
                if (fixed_time_eq32(mac, (const uint8_t*)sig_bytes.data())) {
                  d["_attest_sig_ok"] = true;
                } else {
                  d["_attest_sig_ok"] = false;
                  d["_attest_sig_error"] = "verify_failed";
                }
              }
            }
          } else {
            d["_attest_sig_ok"] = false;
            d["_attest_sig_error"] = "unsupported_alg";
          }
        }
        if (d.isMember("_attest_sig_ok") && d["_attest_sig_ok"].isBool()) {
          attest_sig_ok = d["_attest_sig_ok"].asBool();
          if (!attest_sig_ok && d.isMember("_attest_sig_error") && d["_attest_sig_error"].isString()) {
            attest_sig_error = trim_copy(d["_attest_sig_error"].asString());
          }
        }
      }
    }

    if (apply_update && state == "SUCCEEDED") {
      const bool enforce_attest = cfg.edge_attest_required && tr.mode == "invoke";
      const bool enforce_sig = cfg.edge_attest_require_sig && tr.mode == "invoke";
      if (enforce_attest) d["_attest_required"] = true;
      if (enforce_sig) d["_attest_sig_required"] = true;
      if (enforce_attest || enforce_sig) {
        bool attest_fail = false;
        std::string attest_fail_reason;
        if (enforce_attest) {
          if (!attest_present) {
            attest_fail = true;
            attest_fail_reason = "attest_required";
          } else if (attest_result_sha.empty() || !edge_sha256_token_is_safe(attest_result_sha)) {
            attest_fail = true;
            attest_fail_reason = "attest_missing_result_sha256";
          } else if (attest_result_sha_mismatch) {
            attest_fail = true;
            attest_fail_reason = "attest_result_sha256_mismatch";
          }
        }
        if (!attest_fail && enforce_sig) {
          if (!attest_sig_checked) {
            attest_fail = true;
            attest_fail_reason = "attest_sig_required";
          } else if (!attest_sig_ok) {
            attest_fail = true;
            attest_fail_reason = attest_sig_error.empty() ? "attest_sig_invalid" : ("attest_sig_invalid:" + attest_sig_error);
          }
        }
        if (attest_fail) {
          state_eff = "FAILED";
          error_text_eff = attest_fail_reason;
          d["_attest_error"] = attest_fail_reason;
        }
      }
    }

    const std::string effective_state = apply_update ? state_eff : tr.state;
    if (apply_update) {
      tr.state = state_eff;
      tr.updated_utc_ms = ts_eff;
      if (!result_json.empty() && state_eff != "SUCCEEDED") tr.result_json = result_json;
      if (!error_text_eff.empty()) tr.error = error_text_eff;
      if (can_backfill_trace_id) tr.trace_id = msg_trace_id_eff;
      (void)db_or_null->upsert_edge_task(tr, nullptr);
    } else if (can_backfill_trace_id) {
      tr.trace_id = msg_trace_id_eff;
      tr.updated_utc_ms = std::max<int64_t>(tr.updated_utc_ms, ts_eff);
      (void)db_or_null->upsert_edge_task(tr, nullptr);
    }
    // If the incoming message omitted trace, but the platform has a stored trace_id for this task,
    // inject it so trace correlation remains durable across lossy/legacy transports.
    if (!msg_trace_id_valid && !msg_trace_id.empty()) {
      d["_trace_id_invalid"] = true;
      d["_msg_trace_id"] = msg_trace_id;
      if (d.isMember("trace")) d.removeMember("trace");
    }
    if (msg_trace_id_eff.empty() && !tr.trace_id.empty()) {
      Json::Value trc(Json::objectValue);
      trc["trace_id"] = tr.trace_id;
      d["trace"] = trc;
    } else if (!tr.trace_id.empty() && !msg_trace_id_eff.empty() && msg_trace_id_eff != tr.trace_id) {
      d["_trace_id_mismatch"] = true;
      d["_platform_trace_id"] = tr.trace_id;
      d["_msg_trace_id"] = msg_trace_id_eff;
    }
    if (idempotency_mismatch) {
      d["_ignored_by_platform"] = true;
      d["_reason"] = "idempotency_key_mismatch";
      d["_platform_idempotency_key"] = tr.idempotency_key;
      d["_msg_idempotency_key"] = got_idem;
    } else if (terminal && state != tr.state) {
      d["_ignored_by_platform"] = true;
      d["_platform_state"] = tr.state;
    }
    if (attest_result_sha_mismatch) {
      d["_attest_result_sha256_mismatch"] = true;
      d["_attest_result_sha256"] = attest_result_sha;
      d["_platform_result_sha256"] = tr.result_sha256;
    }
    if (!platform_hash_alg.empty()) d["_platform_result_sha256_alg"] = platform_hash_alg;
    if (!platform_c14n_error.empty()) d["_platform_result_c14n_error"] = platform_c14n_error;
    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = ts_eff;
    ev.state = apply_update ? state_eff : state;
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_task_event(ev, nullptr, nullptr);

    // Best-effort: if this task belongs to an edge workflow (task_id == workflow_id), reflect state into the step.
    if (apply_update || state == effective_state) {
      AgentDb::EdgeWorkflowRow wf;
      std::string werr;
      if (db_or_null->get_edge_workflow(task_id, &wf, &werr)) {
        std::vector<AgentDb::EdgeWorkflowStepRow> steps;
        std::string serr;
        if (db_or_null->list_edge_workflow_steps(task_id, &steps, &serr)) {
          for (auto& s : steps) {
            if (s.step_id != step_id) continue;
            if (s.state != effective_state) {
              s.state = effective_state;
              s.updated_utc_ms = ts_eff;
              if (!error_text_eff.empty()) s.error = error_text_eff;
              (void)db_or_null->upsert_edge_workflow_step(s, nullptr);
            }
            AgentDb::EdgeWorkflowEventRow wev;
            wev.workflow_id = task_id;
            wev.ts_utc_ms = ts_eff;
            wev.type = "step_state";
            Json::Value wd(Json::objectValue);
            wd["workflow_id"] = task_id;
            wd["step_id"] = step_id;
            wd["state"] = effective_state;
            if (!got_idem.empty()) wd["idempotency_key"] = got_idem;
            if (!error_text_eff.empty()) wd["error"] = error_text_eff;
            if (idempotency_mismatch) wd["_ignored_by_platform"] = true;
            wev.data_json = edge_json_stringify_compact(wd);
            (void)db_or_null->insert_edge_workflow_event(wev, nullptr, nullptr);
            break;
          }
        }
      }
    }
  };

  if (type == "TASK_ACK") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_ACK body");
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_ACK body (missing/invalid idempotency_key)");
      return;
    }
    if (!body.isMember("accepted") || !body["accepted"].isBool()) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_ACK body (missing accepted: bool)");
      return;
    }
    const bool accepted = body["accepted"].asBool();
    std::string reason;
    if (body.isMember("reason") && body["reason"].isString()) reason = body["reason"].asString();
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    if (accepted) {
      update_task_state(task_id, step_id, "QUEUED", /*result_json=*/"", /*error_text=*/"", d);
    } else {
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", reason.empty() ? "rejected" : reason, d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_EVENT") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string state = body.isMember("state") && body["state"].isString() ? body["state"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || state.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_EVENT body");
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_EVENT body (missing/invalid idempotency_key)");
      return;
    }
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    std::string result_json;
    if (body.isMember("result")) result_json = edge_json_stringify_compact(body["result"]);
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    update_task_state(task_id, step_id, state, result_json, error, d);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_DONE") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_DONE body");
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_DONE body (missing/invalid idempotency_key)");
      return;
    }
    if (!body.isMember("result")) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_DONE body (missing result)");
      return;
    }

    // Optional correctness guardrail: if the node manifest includes a `result_schema` for this tool,
    // validate `result.data` before marking the task SUCCEEDED (prevents malformed outputs from flowing into workflows).
    //
    // Policy: treat schema mismatch as a terminal FAILED (fail-closed). This is limited to mode=invoke tasks.
    std::string result_schema_error;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (db_or_null && db_or_null->get_edge_task(task_id, step_id, &tr, &terr) && tr.mode == "invoke" && !tr.node_id.empty() &&
        !tr.tool_name.empty()) {
      AgentDb::EdgeNodeRow nr;
      std::string nerr;
      if (db_or_null->get_edge_node(tr.node_id, &nr, &nerr) && !nr.manifest_json.empty()) {
        Json::Value manifest;
        std::string merr;
        if (json_parse_any(nr.manifest_json, &manifest, &merr) && manifest.isObject()) {
          Json::Value schema;
          std::string serr;
          if (edge_tool_result_schema_from_manifest_best_effort(manifest, tr.tool_name, &schema, &serr) && schema.isObject()) {
            const Json::Value result = body["result"];
            if (!result.isObject()) {
              result_schema_error = "result must be an object";
            } else if (!result.isMember("data")) {
              result_schema_error = "missing result.data";
            } else {
              const Json::Value data = result["data"];
              std::string verr;
              if (!edge_json_schema_subset_validate_best_effort(schema, data, "result.data", &verr)) {
                result_schema_error = verr.empty() ? "result_schema mismatch" : verr;
              }
            }
          }
        }
      }
    }

    std::string result_json;
    if (body.isMember("result")) result_json = edge_json_stringify_compact(body["result"]);
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    if (!result_schema_error.empty()) {
      d["_result_schema_error"] = result_schema_error;
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", std::string("result_schema_mismatch: ") + result_schema_error, d);
    } else {
      update_task_state(task_id, step_id, "SUCCEEDED", result_json, /*error_text=*/"", d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_FAILED") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_FAILED body");
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_FAILED body (missing/invalid idempotency_key)");
      return;
    }
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    if (error.empty()) {
      resp->status = 400;
      resp->body = json_error_body("invalid TASK_FAILED body (missing error: string)");
      return;
    }
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", error, d);
    resp->body = "{\"ok\":true}";
    return;
  }

  // Node-initiated collaboration: allow nodes to submit/cancel edge workflows via the same UM‑BMP message ingress.
  //
  // This is a platform-side extension beyond the strict UM‑EAIS v0.1 draft; it enables:
  // - sensor nodes to ask the platform to orchestrate multi-node workflows without pre-configured rules
  // - embedded agents to “handoff” an intent to the platform coordinator
  if (type == "WORKFLOW_SUBMIT") {
    Json::Value wfargs = body;
    if (body.isMember("workflow") && body["workflow"].isObject()) wfargs = body["workflow"];
    if (!wfargs.isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid WORKFLOW_SUBMIT body (expected object)");
      return;
    }

    std::string workflow_id = wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
    if (workflow_id.empty()) workflow_id = std::string("wf:") + edge_make_uuidish_msg_id();
    if (!edge_id_is_safe(workflow_id)) {
      resp->status = 400;
      resp->body = json_error_body("invalid workflow_id");
      return;
    }

    const std::string goal = wfargs.isMember("goal") && wfargs["goal"].isString() ? wfargs["goal"].asString() : "";
    int priority = 0;
    if (wfargs.isMember("priority") && (wfargs["priority"].isInt() || wfargs["priority"].isUInt())) {
      priority = wfargs["priority"].isInt() ? wfargs["priority"].asInt() : (int)std::min((Json::UInt)INT32_MAX, wfargs["priority"].asUInt());
    }

    if (!wfargs.isMember("steps") || !wfargs["steps"].isArray() || wfargs["steps"].empty()) {
      resp->status = 400;
      resp->body = json_error_body("missing/invalid steps (expected non-empty array)");
      return;
    }

    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    steps.reserve(wfargs["steps"].size());

    for (Json::ArrayIndex i = 0; i < wfargs["steps"].size(); i++) {
      const auto& s = wfargs["steps"][i];
      if (!s.isObject()) continue;
      const std::string step_id = s.isMember("step_id") && s["step_id"].isString() ? trim_copy(s["step_id"].asString()) : "";
      const std::string kind = s.isMember("kind") && s["kind"].isString() ? trim_copy(s["kind"].asString()) : "";
      if (step_id.empty() || !edge_id_is_safe(step_id) || kind.empty()) {
        resp->status = 400;
        resp->body = json_error_body("invalid step (missing step_id/kind)");
        return;
      }
      if (kind != "invoke_tool" && kind != "run_agent" && kind != "join") {
        resp->status = 400;
        resp->body = json_error_body("unsupported step.kind");
        return;
      }

      Json::Value depends(Json::arrayValue);
      if (s.isMember("depends_on") && s["depends_on"].isArray()) depends = s["depends_on"];
      Json::Value target = s.isMember("target") ? s["target"] : Json::Value(Json::objectValue);
      Json::Value payload = s.isMember("payload") ? s["payload"] : Json::Value(Json::objectValue);
      if (kind != "join" && !target.isObject()) {
        resp->status = 400;
        resp->body = json_error_body("invalid step.target (expected object)");
        return;
      }
      if (!payload.isObject()) {
        resp->status = 400;
        resp->body = json_error_body("invalid step.payload (expected object)");
        return;
      }

      std::string join_mode = s.isMember("join_mode") && s["join_mode"].isString() ? trim_copy(s["join_mode"].asString()) : "";
      if (!join_mode.empty() && join_mode != "all" && join_mode != "any") {
        resp->status = 400;
        resp->body = json_error_body("invalid join_mode (expected all|any)");
        return;
      }

      int64_t deadline_utc_ms = 0;
      if (s.isMember("deadline_utc_ms") && (s["deadline_utc_ms"].isInt64() || s["deadline_utc_ms"].isUInt64())) {
        deadline_utc_ms = s["deadline_utc_ms"].isInt64() ? s["deadline_utc_ms"].asInt64() : (int64_t)s["deadline_utc_ms"].asUInt64();
      }
      if (kind != "join" && deadline_utc_ms <= 0) deadline_utc_ms = now + 60000;

      int max_attempts = 1;
      if (s.isMember("max_attempts") && (s["max_attempts"].isInt() || s["max_attempts"].isUInt())) {
        max_attempts = s["max_attempts"].isInt() ? s["max_attempts"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["max_attempts"].asUInt());
      }
      if (max_attempts < 1) max_attempts = 1;
      if (max_attempts > 100) max_attempts = 100;

      int backoff_ms = 0;
      if (s.isMember("backoff_ms") && (s["backoff_ms"].isInt() || s["backoff_ms"].isUInt())) {
        backoff_ms = s["backoff_ms"].isInt() ? s["backoff_ms"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["backoff_ms"].asUInt());
      }
      if (backoff_ms < 0) backoff_ms = 0;
      if (backoff_ms > 600000) backoff_ms = 600000;

      AgentDb::EdgeWorkflowStepRow row;
      row.workflow_id = workflow_id;
      row.step_id = step_id;
      row.kind = kind;
      row.depends_on_json = edge_json_stringify_compact(depends);
      row.target_json = edge_json_stringify_compact(target);
      row.payload_json = edge_json_stringify_compact(payload);
      row.join_mode = join_mode;
      row.deadline_utc_ms = deadline_utc_ms;
      row.attempt = 0;
      row.max_attempts = (kind == "join") ? 1 : max_attempts;
      row.next_ready_utc_ms = 0;
      row.backoff_ms = (kind == "join") ? 0 : backoff_ms;
      row.state = "PENDING";
      row.created_utc_ms = now;
      row.updated_utc_ms = now;
      steps.push_back(std::move(row));
    }

    AgentDb::EdgeWorkflowRow wf;
    wf.workflow_id = workflow_id;
    wf.goal = goal;
    wf.status = "QUEUED";
    wf.priority = priority;
    wf.spec_json = edge_json_stringify_compact(wfargs);
    wf.created_utc_ms = now;
    wf.updated_utc_ms = now;

    std::string werr;
    if (!db_or_null->create_edge_workflow(wf, steps, &werr)) {
      AgentDb::EdgeWorkflowRow existing;
      std::string gerr;
      if (!db_or_null->get_edge_workflow(workflow_id, &existing, &gerr)) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to create edge workflow";
        o["detail"] = werr;
        resp->body = edge_json_stringify_compact(o);
        return;
      }
    }

    {
      AgentDb::EdgeWorkflowEventRow ev;
      ev.workflow_id = workflow_id;
      ev.ts_utc_ms = now;
      ev.type = "workflow_created";
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      if (!goal.empty()) d["goal"] = goal;
      d["priority"] = priority;
      if (!from_id.empty()) d["submitted_from"] = from_id;
      d["steps"] = (Json::Int64)steps.size();
      ev.data_json = edge_json_stringify_compact(d);
      (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);
    }

    // Best-effort: send an explicit ACK to the submitting node via outbox, so non-HTTP transports can observe it.
    std::string reply_node_id;
    if (from_id.rfind("node:", 0) == 0) reply_node_id = from_id.substr(5);
    if (body.isMember("node_id") && body["node_id"].isString()) reply_node_id = trim_copy(body["node_id"].asString());
    if (!reply_node_id.empty() && edge_id_is_safe(reply_node_id)) {
      Json::Value ack(Json::objectValue);
      ack["msg_id"] = edge_make_uuidish_msg_id();
      ack["ts_utc_ms"] = (Json::Int64)now;
      ack["type"] = "WORKFLOW_ACK";
      ack["from"] = "platform";
      ack["to"] = edge_node_to_prefix(reply_node_id);
      Json::Value b(Json::objectValue);
      b["workflow_id"] = workflow_id;
      b["ok"] = true;
      ack["body"] = b;
      AgentDb::EdgeOutboxMessageRow orow;
      orow.node_id = reply_node_id;
      orow.ts_utc_ms = now;
      orow.envelope_json = edge_json_stringify_compact(ack);
      (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
    }

    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["workflow_id"] = workflow_id;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  if (type == "WORKFLOW_CANCEL") {
    const std::string workflow_id = body.isMember("workflow_id") && body["workflow_id"].isString()
      ? trim_copy(body["workflow_id"].asString())
      : "";
    if (workflow_id.empty() || !edge_id_is_safe(workflow_id)) {
      resp->status = 400;
      resp->body = json_error_body("missing/invalid workflow_id");
      return;
    }
    AgentDb::EdgeWorkflowRow wf;
    std::string werr;
    if (!db_or_null->get_edge_workflow(workflow_id, &wf, &werr)) {
      resp->status = 404;
      resp->body = json_error_body("workflow not found");
      return;
    }
    if (wf.status != "CANCELED" && wf.status != "SUCCEEDED" && wf.status != "FAILED") {
      wf.status = "CANCELED";
      wf.updated_utc_ms = now;
      (void)db_or_null->upsert_edge_workflow(wf, nullptr);
    }

    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    std::string serr;
    (void)db_or_null->list_edge_workflow_steps(workflow_id, &steps, &serr);
    for (auto& s : steps) {
      if (s.state == "SUCCEEDED" || s.state == "FAILED" || s.state == "TIMED_OUT" || s.state == "CANCELED") continue;
      s.state = "CANCELED";
      s.updated_utc_ms = now;
      (void)db_or_null->upsert_edge_workflow_step(s, nullptr);
    }

    AgentDb::EdgeWorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_utc_ms = now;
    ev.type = "workflow_canceled";
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    if (!from_id.empty()) d["canceled_from"] = from_id;
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);

    // Best-effort: send an explicit ACK to the node via outbox so non-HTTP transports can observe cancellation.
    // (Matches the WORKFLOW_SUBMIT ACK pattern.)
    {
      Json::Value ack_body(Json::objectValue);
      ack_body["workflow_id"] = workflow_id;
      ack_body["ok"] = true;
      ack_body["status"] = "CANCELED";
      try_send_outbox_ack("WORKFLOW_ACK", ack_body);
    }

    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["workflow_id"] = workflow_id;
    o["status"] = wf.status;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  if (type == "SENSOR_EVENT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string event_type = body.isMember("event_type") && body["event_type"].isString() ? body["event_type"].asString() : "";
    const int64_t ts2 = body.isMember("ts_utc_ms") && (body["ts_utc_ms"].isInt64() || body["ts_utc_ms"].isUInt64())
      ? (body["ts_utc_ms"].isInt64() ? body["ts_utc_ms"].asInt64() : (int64_t)body["ts_utc_ms"].asUInt64())
      : (ts_utc_ms > 0 ? ts_utc_ms : now);
    const double confidence = body.isMember("confidence") && (body["confidence"].isDouble() || body["confidence"].isInt())
      ? body["confidence"].asDouble()
      : 0.0;
    Json::Value data = body.isMember("data") ? body["data"] : Json::Value(Json::objectValue);
    if (node_id.empty() || !edge_id_is_safe(node_id) || event_type.empty()) {
      resp->status = 400;
      resp->body = json_error_body("invalid SENSOR_EVENT body");
      return;
    }
    AgentDb::EdgeSensorEventRow sr;
    sr.node_id = node_id;
    sr.event_type = event_type;
    sr.ts_utc_ms = ts2;
    sr.confidence = confidence;
    sr.data_json = edge_json_stringify_compact(data.isNull() ? Json::Value(Json::objectValue) : data);
    (void)db_or_null->insert_edge_sensor_event(sr, nullptr, nullptr);

    edge_rules_apply_for_sensor_event_best_effort(
      cfg,
      cors_cfg,
      db_or_null,
      req,
      node_id,
      msg_id,
      event_type,
      ts2,
      confidence,
      data
    );
    resp->body = "{\"ok\":true}";
    return;
  }

  // Unknown message types are accepted and persisted (forward compatible).
  resp->body = "{\"ok\":true}";
}

void handle_edge_outbox_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    return;
  }

  const std::string accept =
    req.headers.count("accept") ? trim_copy(req.headers.at("accept")) : "";
  const bool want_cbor = !accept.empty() && url_contains_ci(accept, "application/cbor");

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    return;
  }

  int64_t cursor = 0;
  const auto c = query_get(req.query, "cursor");
  if (c && !c->empty()) {
    try { cursor = (int64_t)std::stoll(*c); } catch (...) { cursor = 0; }
  }
  if (cursor < 0) cursor = 0;

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 2048));

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  std::string err;
  if (!db_or_null->list_edge_outbox_messages(*nid, cursor, limit, &msgs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list outbox";
    o["detail"] = err;
    if (want_cbor) {
      std::string bytes;
      std::string cerr;
      if (cbor_encode_json_value(o, &bytes, &cerr)) {
        resp->headers["Content-Type"] = "application/cbor";
        resp->body = bytes;
        return;
      }
    }
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& m : msgs) {
    Json::Value row(Json::objectValue);
    row["outbox_id"] = (Json::Int64)m.outbox_id;
    row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
    Json::Value env;
    std::string perr2;
    if (json_parse_any(m.envelope_json, &env, &perr2) && env.isObject()) {
      row["msg"] = env;
    } else {
      row["msg_raw"] = m.envelope_json;
      row["parse_error"] = perr2;
    }
    arr.append(row);
    cursor_next = std::max(cursor_next, m.outbox_id);
  }
  o["messages"] = arr;
  o["cursor_next"] = (Json::Int64)cursor_next;
  if (want_cbor) {
    std::string bytes;
    std::string cerr;
    if (cbor_encode_json_value(o, &bytes, &cerr)) {
      resp->headers["Content-Type"] = "application/cbor";
      resp->body = bytes;
      return;
    }
    // Fall back to JSON on encoding errors.
  }
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_nodes_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db_or_null->list_edge_nodes(limit, &nodes, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list nodes";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  std::unordered_set<std::string> seen_node_ids;
  seen_node_ids.reserve(nodes.size());
  for (const auto& n : nodes) {
    Json::Value runtime = edge_consensus_runtime_status_json_for_node(cfg, db_or_null, n.node_id);
    Json::Value row = build_edge_node_summary_json(&n, runtime, n.node_id);
    arr.append(row);
    seen_node_ids.insert(n.node_id);
  }
  append_runtime_only_edge_node_summaries(cfg, db_or_null, limit, &seen_node_ids, &arr);
  o["nodes"] = arr;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  Json::Value runtime = edge_consensus_runtime_status_json_for_node(cfg, db_or_null, *nid);
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    if (!runtime.isObject()) {
      resp->status = 404;
      resp->body = json_error_body("node not found");
      return;
    }
    Json::Value row = build_edge_node_summary_json(nullptr, runtime, *nid);
    append_edge_node_detail_json(cfg, nullptr, &row);
    o["node"] = row;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value row = build_edge_node_summary_json(&n, runtime, *nid);
  append_edge_node_detail_json(cfg, &n, &row);
  o["node"] = row;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = json_error_body("node not found");
    return;
  }
  if (n.manifest_json.empty()) {
    resp->status = 404;
    resp->body = json_error_body("node has no manifest");
    return;
  }

  Json::Value m;
  std::string perr;
  if (!json_parse_any(n.manifest_json, &m, &perr) || !m.isObject()) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to parse stored manifest";
    o["parse_error"] = perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["manifest"] = m;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_manifest_bundle_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  Json::Value bundle;
  std::string sign_err;
  if (!build_edge_node_manifest_bundle(cfg, db_or_null, *nid, &bundle, &sign_err)) {
    if (sign_err == "node not found" || sign_err == "node has no manifest") {
      resp->status = 404;
      resp->body = json_error_body(sign_err);
      return;
    }
    if (sign_err == "failed to parse stored manifest" || sign_err == "failed to hash manifest") {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = sign_err;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (sign_err.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(sign_err.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (sign_err.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(sign_err.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = "manifest_bundle_sign_failed";
      if (!sign_err.empty()) o["details"] = sign_err;
    }
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["bundle"] = bundle;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_manifest_bundle_send_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  const std::string target_node_id =
    args.isMember("target_node_id") && args["target_node_id"].isString() ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string subject_node_id =
    args.isMember("subject_node_id") && args["subject_node_id"].isString() ? trim_copy(args["subject_node_id"].asString()) : "";
  const std::string confidential_kid =
    args.isMember("confidential_kid") && args["confidential_kid"].isString() ? trim_copy(args["confidential_kid"].asString()) : "";
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id) || subject_node_id.empty() || !edge_id_is_safe(subject_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid target_node_id or subject_node_id");
    return;
  }
  if (!confidential_kid.empty() && (confidential_kid.size() > 64 || !edge_id_is_safe(confidential_kid))) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid confidential_kid");
    return;
  }

  AgentDb::EdgeNodeRow target_row;
  std::string terr;
  if (!db_or_null->get_edge_node(target_node_id, &target_row, &terr)) {
    resp->status = 404;
    resp->body = json_error_body("target node not found");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_node_manifest_bundle(cfg, db_or_null, subject_node_id, &bundle, &berr)) {
    if (berr == "node not found" || berr == "node has no manifest") {
      resp->status = 404;
      resp->body = json_error_body(berr == "node not found" ? "subject node not found" : "subject node has no manifest");
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "manifest_bundle_send_failed" : berr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  env["type"] = "PLATFORM_MANIFEST_BUNDLE";
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(target_node_id);
  Json::Value body(Json::objectValue);
  body["subject_node_id"] = subject_node_id;
  body["bundle"] = bundle;
  env["body"] = body;
  if (!confidential_kid.empty()) {
    std::string ecode;
    std::string eerr;
    if (!edge_confidentiality_wrap_envelope_body(
          &env, cfg.edge_confidentiality_keys, confidential_kid, &ecode, &eerr)) {
      resp->status = (ecode == "unknown_confidential_kid") ? 400 : 500;
      resp->body = json_error_body(eerr.empty() ? "failed to encrypt manifest bundle" : eerr);
      return;
    }
  }

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = target_node_id;
  orow.ts_utc_ms = env["ts_utc_ms"].asInt64();
  orow.envelope_json = edge_json_stringify_compact(env);
  int64_t outbox_id = 0;
  std::string oerr;
  if (!db_or_null->insert_edge_outbox_message(orow, &outbox_id, &oerr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue manifest bundle" : oerr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["target_node_id"] = target_node_id;
  o["subject_node_id"] = subject_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["bundle"] = bundle;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_task_assign_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

	  std::string node_id = args.isMember("node_id") && args["node_id"].isString() ? trim_copy(args["node_id"].asString()) : "";
	  std::vector<std::string> requires_tools;
	  std::vector<std::string> tags_all;
	  std::vector<std::string> tags_any;
	  std::vector<std::string> tags_none;
	  std::unordered_set<std::string> exclude_node_ids;
	  if (node_id.empty() && args.isMember("match_any") && args["match_any"].isObject()) {
	    const auto& m = args["match_any"];
	    auto read_arr = [&](const char* k, std::vector<std::string>* out) {
	      if (!out) return;
      out->clear();
      if (!m.isMember(k) || !m[k].isArray()) return;
      for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
        if (!m[k][i].isString()) continue;
        out->push_back(m[k][i].asString());
      }
    };
	    read_arr("requires_tools", &requires_tools);
	    read_arr("tags_all", &tags_all);
	    read_arr("tags_any", &tags_any);
	    read_arr("tags_none", &tags_none);
	    if (m.isMember("exclude_node_ids") && m["exclude_node_ids"].isArray()) {
	      for (Json::ArrayIndex i = 0; i < m["exclude_node_ids"].size(); i++) {
	        if (!m["exclude_node_ids"][i].isString()) continue;
	        const std::string s = trim_copy(m["exclude_node_ids"][i].asString());
	        if (!s.empty()) exclude_node_ids.insert(s);
	      }
	    }

	    const std::unordered_set<std::string>* ex = exclude_node_ids.empty() ? nullptr : &exclude_node_ids;
	    if (!select_node_match_any(db_or_null, requires_tools, tags_all, tags_any, tags_none, ex, &node_id)) {
	      resp->status = 409;
	      resp->body = json_error_body("no matching node");
	      return;
	    }
	  }
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id (or match_any did not select)");
    return;
  }

  const std::string task_id = args.isMember("task_id") && args["task_id"].isString() ? args["task_id"].asString() : "";
  const std::string step_id = args.isMember("step_id") && args["step_id"].isString() ? args["step_id"].asString() : "";
  const std::string idempotency_key =
    args.isMember("idempotency_key") && args["idempotency_key"].isString() ? args["idempotency_key"].asString() : "";
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? args["mode"].asString() : "";
  const int64_t deadline_utc_ms = args.isMember("deadline_utc_ms") && (args["deadline_utc_ms"].isInt64() || args["deadline_utc_ms"].isUInt64())
    ? (args["deadline_utc_ms"].isInt64() ? args["deadline_utc_ms"].asInt64() : (int64_t)args["deadline_utc_ms"].asUInt64())
    : 0;
  const int attempt = args.isMember("attempt") && (args["attempt"].isInt() || args["attempt"].isUInt())
    ? (args["attempt"].isInt() ? args["attempt"].asInt() : (int)std::min((Json::UInt)INT32_MAX, args["attempt"].asUInt()))
    : 0;
  const Json::Value payload = args.isMember("payload") ? args["payload"] : Json::Value(Json::nullValue);
  const Json::Value trace = args.isMember("trace") ? args["trace"] : Json::Value(Json::nullValue);

  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid task fields");
    return;
  }
  if (!trace.isNull() && !trace.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("invalid trace (expected object)");
    return;
  }

  std::unordered_set<std::string> allow_hazards;
  if (args.isMember("allow_hazards") && args["allow_hazards"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["allow_hazards"].size(); i++) {
      if (args["allow_hazards"][i].isString()) allow_hazards.insert(args["allow_hazards"][i].asString());
    }
  }
  const bool allow_high_side_effect =
    args.isMember("allow_high_side_effect") && args["allow_high_side_effect"].isBool() ? args["allow_high_side_effect"].asBool() : false;

  int64_t outbox_id = 0;
  bool deduped = false;
  std::string derr;
  int http = 500;
  if (!edge_enqueue_task_assign(
        db_or_null,
        node_id,
        task_id,
        step_id,
        idempotency_key,
        mode,
        deadline_utc_ms,
        attempt,
        payload,
        trace,
        allow_hazards,
        allow_high_side_effect,
        /*enforce_safety=*/true,
        /*enforce_rate_limit=*/true,
        &outbox_id,
        &deduped,
        &derr,
        &http)) {
    resp->status = http;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = derr.empty() ? "failed to assign edge task" : derr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = node_id;
  o["task_id"] = task_id;
  o["step_id"] = step_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  o["deduped"] = deduped;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_task_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto tid = query_get(req.query, "task_id");
  const auto sid = query_get(req.query, "step_id");
  if (!tid || tid->empty() || !sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing task_id/step_id");
    return;
  }

  AgentDb::EdgeTaskRow tr;
  std::string err;
  if (!db_or_null->get_edge_task(*tid, *sid, &tr, &err)) {
    resp->status = 404;
    resp->body = json_error_body("task not found");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value t(Json::objectValue);
  t["task_id"] = tr.task_id;
  t["step_id"] = tr.step_id;
  t["node_id"] = tr.node_id;
  t["idempotency_key"] = tr.idempotency_key;
  if (!tr.trace_id.empty()) t["trace_id"] = tr.trace_id;
  if (!tr.result_sha256.empty()) t["result_sha256"] = tr.result_sha256;
  t["mode"] = tr.mode;
  if (!tr.tool_name.empty()) t["tool_name"] = tr.tool_name;
  if (!tr.resource_lock.empty()) t["resource_lock"] = tr.resource_lock;
  t["deadline_utc_ms"] = (Json::Int64)tr.deadline_utc_ms;
  t["state"] = tr.state;
  t["created_utc_ms"] = (Json::Int64)tr.created_utc_ms;
  t["updated_utc_ms"] = (Json::Int64)tr.updated_utc_ms;
  if (!tr.error.empty()) t["error"] = tr.error;
  if (!tr.payload_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.payload_json, &v, &perr2) && v.isObject()) t["payload"] = v;
  }
  if (!tr.result_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.result_json, &v, &perr2)) t["result"] = v;
  }
  if (!tr.attest_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.attest_json, &v, &perr2) && v.isObject()) t["attest"] = v;
  }
  o["task"] = t;
  resp->body = edge_json_stringify_compact(o);
}

}  // namespace agentd
