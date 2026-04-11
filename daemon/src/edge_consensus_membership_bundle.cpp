#include "edge_consensus_membership_bundle.h"

#include "base64.h"
#include "daemon_config.h"
#include "edge_util.h"
#include "string_util.h"

#include "agent/ed25519.h"
#include "agent/edge_interop.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include <array>
#include <cstdint>

namespace agentd {
namespace {

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

}  // namespace

bool build_edge_consensus_membership_bundle(
  const DaemonConfig& cfg,
  const std::string& cluster_id,
  Json::Value* out_bundle,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bundle) return false;
  if (!edge_id_is_safe(cluster_id)) {
    if (out_error) *out_error = "invalid cluster_id";
    return false;
  }
  const auto it = cfg.edge_consensus_clusters.find(cluster_id);
  if (it == cfg.edge_consensus_clusters.end() || it->second.member_node_ids.empty()) {
    if (out_error) *out_error = "consensus_membership_unavailable";
    return false;
  }

  const EdgeConsensusClusterPolicy& pol = it->second;
  Json::Value bundle(Json::objectValue);
  bundle["schema"] = AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1;
  bundle["created_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  bundle["cluster_id"] = cluster_id;
  bundle["membership_epoch"] = (Json::Int64)pol.membership_epoch;
  bundle["previous_membership_epoch"] = (Json::Int64)pol.previous_membership_epoch;
  bundle["updated_utc_ms"] = (Json::Int64)pol.updated_utc_ms;
  bundle["campaign_delay_ms"] = (Json::Int64)pol.campaign_delay_ms;
  bundle["campaign_retry_ms"] = (Json::Int64)pol.campaign_retry_ms;
  bundle["campaign_retry_max_ms"] = (Json::Int64)pol.campaign_retry_max_ms;
  bundle["campaign_retry_backoff_factor"] = (Json::Int64)pol.campaign_retry_backoff_factor;
  bundle["leader_heartbeat_ms"] = (Json::Int64)pol.leader_heartbeat_ms;
  bundle["leader_lease_ms"] = (Json::Int64)pol.leader_lease_ms;
  bundle["lease_expiry_recampaign_delay_ms"] = (Json::Int64)pol.lease_expiry_recampaign_delay_ms;
  bundle["stale_runtime_recovery_grace_ms"] = (Json::Int64)pol.stale_runtime_recovery_grace_ms;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& member : pol.member_node_ids) arr.append(member);
    bundle["member_node_ids"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& member : pol.previous_member_node_ids) arr.append(member);
    bundle["previous_member_node_ids"] = arr;
  }
  {
    Json::Value lineage(Json::arrayValue);
    size_t lineage_count = 0;
    for (const auto& entry : pol.membership_lineage) {
      if (entry.membership_epoch <= 0 || entry.member_node_ids.empty()) continue;
      Json::Value item(Json::objectValue);
      item["membership_epoch"] = (Json::Int64)entry.membership_epoch;
      Json::Value members(Json::arrayValue);
      for (const auto& member : entry.member_node_ids) members.append(member);
      item["member_node_ids"] = members;
      lineage.append(item);
      lineage_count++;
      if (lineage_count >= AGENT_EDGE_CONSENSUS_MEMBERSHIP_LINEAGE_MAX) break;
    }
    bundle["membership_lineage"] = lineage;
  }

  const bool hmac_kid_set = !cfg.run_attest_hmac_kid.empty();
  const bool hmac_key_set = !cfg.run_attest_hmac_key.empty();
  const bool ed_kid_set = !cfg.run_attest_ed25519_kid.empty();
  const bool ed_seed_set = !cfg.run_attest_ed25519_seed.empty();
  const int sign_modes = (hmac_kid_set || hmac_key_set ? 1 : 0) + (ed_kid_set || ed_seed_set ? 1 : 0);
  if (sign_modes == 0) {
    *out_bundle = bundle;
    return true;
  }
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

  std::string canon;
  std::string cerr;
  if (!canonical_json_bytes(bundle, &canon, &cerr)) {
    if (out_error) *out_error = std::string("attest_sign_failed: ") + (cerr.empty() ? "c14n_failed" : cerr);
    return false;
  }
  Json::Value att(Json::objectValue);
  att["schema"] = AGENT_EDGE_CONSENSUS_MEMBERSHIP_ATTEST_SCHEMA_V1;
  att["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  att["cluster_id"] = cluster_id;
  att["membership_epoch"] = (Json::Int64)pol.membership_epoch;
  att["previous_membership_epoch"] = (Json::Int64)pol.previous_membership_epoch;
  att["signing_schema"] = AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1;

  if (hmac_kid_set) {
    std::array<uint8_t, 32> mac{};
    agent_hmac_sha256(cfg.run_attest_hmac_key.data(), cfg.run_attest_hmac_key.size(), canon.data(), canon.size(), mac.data());
    att["alg"] = "hmac-sha256";
    att["kid"] = cfg.run_attest_hmac_kid;
    att["sig"] = base64_encode(reinterpret_cast<const char*>(mac.data()), mac.size());
  } else {
    std::vector<uint8_t> seed;
    std::string serr;
    if (!parse_seed_bytes(cfg.run_attest_ed25519_seed, &seed, &serr)) {
      if (out_error) *out_error = std::string("attest_sign_failed: ") + (serr.empty() ? "invalid_ed25519_seed" : serr);
      return false;
    }
    uint8_t pk[32] = {0};
    uint8_t sig_bytes[64] = {0};
    agent_ed25519_publickey(seed.data(), pk);
    agent_ed25519_sign(canon.data(), canon.size(), seed.data(), pk, sig_bytes);
    att["alg"] = "ed25519";
    att["kid"] = cfg.run_attest_ed25519_kid;
    att["pubkey"] = base64_encode(reinterpret_cast<const char*>(pk), sizeof(pk));
    att["sig"] = base64_encode(reinterpret_cast<const char*>(sig_bytes), sizeof(sig_bytes));
  }
  bundle["attest"] = att;
  *out_bundle = bundle;
  return true;
}

}  // namespace agentd
