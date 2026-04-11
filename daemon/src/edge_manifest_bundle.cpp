#include "edge_manifest_bundle.h"

#include "agent_db.h"
#include "base64.h"
#include "config_endpoint.h"
#include "daemon_config.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include "agent/ed25519.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace agentd {
namespace {

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
    for (const auto& s : result.chain_subjects) {
      if (!s.empty()) arr.append(s);
    }
    (*out)["verified_chain_subjects"] = arr;
  }
  if (!result.matched_root_kids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.matched_root_kids) {
      if (!s.empty()) arr.append(s);
    }
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

}  // namespace

bool build_edge_manifest_identity_cert_verify(
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

bool build_edge_node_manifest_bundle(
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
    if (build_edge_manifest_identity_cert_verify(cfg, manifest, &verify, &have_identity_cert, &verr) && have_identity_cert &&
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

}  // namespace agentd
