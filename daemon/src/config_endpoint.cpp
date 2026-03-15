#include "config_endpoint.h"

#include "daemon_auth.h"
#include "edge_confidentiality.h"
#include "edge_runtime_endpoints.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "policy_hooks.h"
#include "runtime_config.h"
#include "provider_util.h"
#include "mount_allowlist.h"
#include "session_voice_runtime.h"
#include "string_util.h"

#include "agent/ed25519.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include "base64.h"

#include <json/json.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include "net_compat.h"

#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace agentd {

namespace {

static Json::Value config_audio_webrtc_metadata_json(const DaemonConfig& cfg) {
  Json::Value out = session_voice_webrtc_backend_metadata_json(cfg);
  out["default_runtime_kind"] = cfg.audio_webrtc_default_runtime_kind.empty()
    ? Json::Value(Json::nullValue)
    : Json::Value(cfg.audio_webrtc_default_runtime_kind);
  out["default_runtime_kind_source"] = cfg.audio_webrtc_default_runtime_kind.empty()
    ? Json::Value("auto")
    : Json::Value(cfg.audio_webrtc_default_runtime_kind_from_env ? "env" : "config");
  out["peer_tool_path_configured"] = out["tool_configured"];
  out.removeMember("tool_configured");
  out.removeMember("tool_path");
  out.removeMember("bundled_tool_path");
  return out;
}

static Json::Value config_edge_consensus_metadata_json(const DaemonConfig& cfg) {
  Json::Value out = edge_consensus_runtime_backend_metadata_json(cfg);
  out["node_tool_path_configured"] = out["tool_configured"];
  out.removeMember("tool_configured");
  out.removeMember("tool_path");
  out.removeMember("default_daemon_url");
  return out;
}

static bool is_safe_tool_name(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 128) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static bool validate_cidr_token_best_effort(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  const size_t slash = s.find('/');
  if (slash == std::string::npos) return false;
  const std::string host = trim_copy(s.substr(0, slash));
  const std::string pref_s = trim_copy(s.substr(slash + 1));
  if (host.empty() || pref_s.empty()) return false;
  int pref = 0;
  try {
    pref = std::stoi(pref_s);
  } catch (...) {
    return false;
  }
  uint8_t buf[16];
  if (::inet_pton(AF_INET, host.c_str(), buf) == 1) {
    return pref >= 0 && pref <= 32;
  }
  if (::inet_pton(AF_INET6, host.c_str(), buf) == 1) {
    return pref >= 0 && pref <= 128;
  }
  return false;
}

static bool validate_hostport_token_best_effort(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 512) return false;
  // No whitespace.
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
  }
  // Allow:
  // - host
  // - host:port (single ':')
  // - [ipv6] or [ipv6]:port
  std::string host;
  std::string port_s;
  if (!s.empty() && s[0] == '[') {
    const size_t rb = s.find(']');
    if (rb == std::string::npos) return false;
    host = s.substr(1, rb - 1);
    if (rb + 1 < s.size()) {
      if (s[rb + 1] != ':') return false;
      port_s = s.substr(rb + 2);
    }
  } else {
    const size_t col = s.rfind(':');
    if (col != std::string::npos && s.find(':') == col) {
      host = s.substr(0, col);
      port_s = s.substr(col + 1);
    } else {
      host = s;
    }
  }
  host = trim_copy(host);
  port_s = trim_copy(port_s);
  if (host.empty()) return false;
  if (!port_s.empty()) {
    int p = 0;
    try {
      p = std::stoi(port_s);
    } catch (...) {
      return false;
    }
    if (p < 1 || p > 65535) return false;
  }
  return true;
}

static bool validate_tool_name_best_effort(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static bool is_known_provider_name(const std::string& s) {
  return s == "deepseek" || s == "openrouter" || s == "moonshot" || s == "openai";
}

static void upsert_tool_call_limit(std::vector<std::pair<std::string, size_t>>* limits, std::string tool, size_t max_calls) {
  if (!limits || tool.empty()) return;
  for (auto& kv : *limits) {
    if (kv.first == tool) {
      kv.second = max_calls;
      return;
    }
  }
  limits->push_back(std::make_pair(std::move(tool), max_calls));
}

static bool read_string_array_best_effort(
  const Json::Value& obj,
  const char* k,
  std::vector<std::string>* out,
  size_t max_n,
  size_t max_len,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  if (!k || !obj.isMember(k)) return true;
  const Json::Value& v = obj[k];
  if (v.isNull()) return true;
  if (!v.isArray()) {
    if (out_err) *out_err = std::string(k) + " must be an array";
    return false;
  }
  if (v.size() > (Json::ArrayIndex)max_n) {
    if (out_err) *out_err = std::string(k) + " too large";
    return false;
  }
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (!v[i].isString()) continue;
    std::string s = trim_copy(v[i].asString());
    if (s.empty()) continue;
    if (s.size() > max_len) {
      if (out_err) *out_err = std::string(k) + " entry too long";
      return false;
    }
    out->push_back(std::move(s));
  }
  return true;
}

static int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
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

static bool validate_edge_kid(const std::string& kid) {
  return !kid.empty() && kid.size() <= 64 && edge_id_is_safe(kid);
}

static bool edge_kid_policy_allows(const std::string& policy_in, const std::string& node_id, const std::string& kid) {
  const std::string policy = trim_copy(policy_in.empty() ? "any" : policy_in);
  if (policy == "any") return true;
  if (policy == "match_node") return kid == node_id;
  if (policy == "node_prefix") {
    const std::string pref = node_id + ":";
    return kid == node_id || (kid.size() > pref.size() && kid.rfind(pref, 0) == 0);
  }
  return false;
}

static bool string_vec_contains(const std::vector<std::string>& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  for (const auto& item : haystack) {
    if (item == needle) return true;
  }
  return false;
}

static Json::Value edge_auth_node_binding_json(const DaemonConfig& cfg, const std::string& node_id) {
  Json::Value out(Json::objectValue);
  out["node_id"] = node_id;
  out["kid_policy"] = cfg.edge_auth_kid_policy;
  out["trust_roots_epoch"] = (Json::Int64)cfg.edge_auth_trust_roots_epoch;
  out["trust_roots_updated_utc_ms"] = (Json::Int64)cfg.edge_auth_trust_roots_updated_utc_ms;
  out["revocations_epoch"] = (Json::Int64)cfg.edge_auth_revocations_epoch;
  out["revocations_updated_utc_ms"] = (Json::Int64)cfg.edge_auth_revocations_updated_utc_ms;
  out["recommended_hmac_kid"] = node_id;
  out["recommended_ed25519_kid"] = node_id;
  const bool node_revoked = string_vec_contains(cfg.edge_auth_revoked_node_ids, node_id);
  out["node_id_revoked"] = node_revoked;

  Json::Value hmac_matches(Json::arrayValue);
  Json::Value ed_matches(Json::arrayValue);
  Json::Value revoked_hmac_matches(Json::arrayValue);
  Json::Value revoked_ed_matches(Json::arrayValue);
  for (const auto& it : cfg.edge_auth_hmac_keys) {
    if (!it.first.empty() && !it.second.empty() && edge_kid_policy_allows(cfg.edge_auth_kid_policy, node_id, it.first)) {
      if (string_vec_contains(cfg.edge_auth_revoked_kids, it.first)) revoked_hmac_matches.append(it.first);
      else hmac_matches.append(it.first);
    }
  }
  for (const auto& it : cfg.edge_auth_ed25519_pubkeys) {
    if (!it.first.empty() && !it.second.empty() && edge_kid_policy_allows(cfg.edge_auth_kid_policy, node_id, it.first)) {
      if (string_vec_contains(cfg.edge_auth_revoked_kids, it.first)) revoked_ed_matches.append(it.first);
      else ed_matches.append(it.first);
    }
  }
  out["matching_hmac_kids"] = hmac_matches;
  out["matching_ed25519_kids"] = ed_matches;
  out["revoked_matching_hmac_kids"] = revoked_hmac_matches;
  out["revoked_matching_ed25519_kids"] = revoked_ed_matches;
  out["kid_policy_satisfied"] = !node_revoked && (hmac_matches.size() > 0 || ed_matches.size() > 0);
  return out;
}

static bool build_edge_auth_trust_roots_bundle(
  const DaemonConfig& cfg,
  Json::Value* out_bundle,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bundle) return false;

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "edge_auth_trust_roots_v1";
  bundle["created_utc_ms"] = (Json::Int64)now_utc_ms();
  bundle["rotation_epoch"] = (Json::Int64)cfg.edge_auth_trust_roots_epoch;
  bundle["updated_utc_ms"] = (Json::Int64)cfg.edge_auth_trust_roots_updated_utc_ms;
  bundle["auth_required"] = cfg.edge_auth_required;
  bundle["require_ts"] = cfg.edge_auth_require_ts;
  bundle["max_skew_ms"] = (Json::Int64)cfg.edge_auth_max_skew_ms;
  bundle["require_seq"] = cfg.edge_auth_require_seq;
  bundle["kid_policy"] = cfg.edge_auth_kid_policy;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& it : cfg.edge_auth_hmac_keys) {
      if (!it.first.empty() && !it.second.empty()) arr.append(it.first);
    }
    bundle["hmac_kids"] = arr;
  }
  {
    Json::Value dir(Json::objectValue);
    for (const auto& it : cfg.edge_auth_ed25519_pubkeys) {
      if (!it.first.empty() && !it.second.empty()) dir[it.first] = it.second;
    }
    bundle["ed25519_pubkeys"] = dir;
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
  att["schema"] = "edge_auth_trust_roots_attest_v1";
  att["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  att["rotation_epoch"] = (Json::Int64)cfg.edge_auth_trust_roots_epoch;
  att["signing_schema"] = "edge_auth_trust_roots_v1";

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

static bool validate_edge_pem_cert_blob_best_effort(const std::string& pem_in, std::string* out_error) {
  if (out_error) out_error->clear();
  const std::string pem = trim_copy(pem_in);
  if (pem.empty()) {
    if (out_error) *out_error = "empty PEM";
    return false;
  }
  if (pem.size() > 128 * 1024) {
    if (out_error) *out_error = "PEM blob too large";
    return false;
  }
  const std::string begin = "-----BEGIN CERTIFICATE-----";
  const std::string end = "-----END CERTIFICATE-----";
  const size_t first_begin = pem.find(begin);
  const size_t first_end = pem.find(end);
  if (first_begin == std::string::npos || first_end == std::string::npos || first_end <= first_begin) {
    if (out_error) *out_error = "missing PEM certificate markers";
    return false;
  }
  size_t count = 0;
  size_t pos = 0;
  while ((pos = pem.find(begin, pos)) != std::string::npos) {
    count++;
    pos += begin.size();
  }
  if (count == 0) {
    if (out_error) *out_error = "missing PEM certificate markers";
    return false;
  }
  return true;
}

static std::string openssl_last_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

static std::string x509_name_to_string(X509_NAME* name) {
  if (!name) return "";
  char* raw = X509_NAME_oneline(name, nullptr, 0);
  if (!raw) return "";
  std::string out = raw;
  OPENSSL_free(raw);
  return out;
}

static std::string x509_sha256_hex(X509* cert) {
  if (!cert) return "";
  unsigned char md[EVP_MAX_MD_SIZE] = {0};
  unsigned int md_len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1 || md_len == 0) return "";
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(md_len * 2);
  for (unsigned int i = 0; i < md_len; i++) {
    out.push_back(hex[(md[i] >> 4) & 0xf]);
    out.push_back(hex[md[i] & 0xf]);
  }
  return out;
}

static void free_x509_stack_only(STACK_OF(X509)* s) {
  if (s) sk_X509_free(s);
}

static void free_x509_stack_with_certs(STACK_OF(X509)* s) {
  if (s) sk_X509_pop_free(s, X509_free);
}

static bool parse_x509_pem_chain(
  const std::string& pem,
  std::vector<std::unique_ptr<X509, decltype(&X509_free)>>* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return false;
  out->clear();
  if (pem.empty()) {
    if (out_error) *out_error = "empty PEM";
    return false;
  }
  ERR_clear_error();
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), (int)pem.size()), BIO_free);
  if (!bio) {
    if (out_error) *out_error = std::string("internal: ") + openssl_last_error_text();
    return false;
  }
  bool saw = false;
  while (true) {
    ERR_clear_error();
    X509* cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr);
    if (cert) {
      saw = true;
      out->emplace_back(cert, X509_free);
      continue;
    }
    const unsigned long err = ERR_peek_last_error();
    if (err == 0) break;
    if (!saw) {
      if (out_error) *out_error = std::string("invalid PEM certificate: ") + openssl_last_error_text();
      ERR_clear_error();
      return false;
    }
    ERR_clear_error();
    break;
  }
  if (!saw) {
    if (out_error) *out_error = "invalid PEM certificate: no certificates found";
    return false;
  }
  return true;
}

struct EdgeRootCertEntry {
  std::string kid;
  std::string sha256_hex;
  std::unique_ptr<X509, decltype(&X509_free)> cert{nullptr, X509_free};
};

static bool load_edge_root_cert_entries(
  const DaemonConfig& cfg,
  std::vector<EdgeRootCertEntry>* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return false;
  out->clear();
  for (const auto& it : cfg.edge_auth_cert_roots_pem) {
    if (it.first.empty() || it.second.empty()) continue;
    std::vector<std::unique_ptr<X509, decltype(&X509_free)>> parsed;
    std::string perr;
    if (!parse_x509_pem_chain(it.second, &parsed, &perr)) {
      if (out_error) *out_error = "invalid configured cert root '" + it.first + "': " + perr;
      return false;
    }
    for (auto& cert : parsed) {
      EdgeRootCertEntry row;
      row.kid = it.first;
      row.sha256_hex = x509_sha256_hex(cert.get());
      row.cert.reset(cert.release());
      out->push_back(std::move(row));
    }
  }
  return true;
}

static bool verify_edge_cert_chain_against_roots_impl(
  const DaemonConfig& cfg,
  const std::string& cert_pem,
  const std::vector<std::string>& untrusted_cert_pem,
  EdgeVerifyChainResult* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_result) return false;
  *out_result = EdgeVerifyChainResult{};

  std::vector<std::unique_ptr<X509, decltype(&X509_free)>> leafs;
  std::string lerr;
  if (!parse_x509_pem_chain(cert_pem, &leafs, &lerr)) {
    if (out_error) *out_error = "invalid cert_pem: " + lerr;
    return false;
  }
  if (leafs.size() != 1) {
    if (out_error) *out_error = "invalid cert_pem: expected exactly one certificate";
    return false;
  }
  X509* leaf = leafs.front().get();
  out_result->leaf_subject = x509_name_to_string(X509_get_subject_name(leaf));
  out_result->leaf_issuer = x509_name_to_string(X509_get_issuer_name(leaf));
  out_result->leaf_sha256_hex = x509_sha256_hex(leaf);

  std::vector<std::unique_ptr<X509, decltype(&X509_free)>> chain_certs;
  for (const auto& pem : untrusted_cert_pem) {
    std::vector<std::unique_ptr<X509, decltype(&X509_free)>> parsed;
    std::string perr;
    if (!parse_x509_pem_chain(pem, &parsed, &perr)) {
      if (out_error) *out_error = "invalid untrusted_cert_pem: " + perr;
      return false;
    }
    for (auto& cert : parsed) chain_certs.emplace_back(cert.release(), X509_free);
  }

  std::vector<EdgeRootCertEntry> roots;
  std::string rerr;
  if (!load_edge_root_cert_entries(cfg, &roots, &rerr)) {
    if (out_error) *out_error = rerr.rfind("invalid configured", 0) == 0 ? rerr : std::string("internal: ") + rerr;
    return false;
  }
  if (roots.empty()) {
    out_result->verified = false;
    out_result->verify_error_code = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
    out_result->verify_error = "no_trusted_roots_configured";
    return true;
  }

  std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
  if (!store) {
    if (out_error) *out_error = std::string("internal: failed to allocate x509 store: ") + openssl_last_error_text();
    return false;
  }
  for (const auto& root : roots) {
    if (!root.cert) continue;
    ERR_clear_error();
    if (X509_STORE_add_cert(store.get(), root.cert.get()) != 1) {
      const unsigned long err = ERR_peek_last_error();
      if (ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        if (out_error) *out_error = std::string("internal: failed to add trusted cert: ") + openssl_last_error_text();
        ERR_clear_error();
        return false;
      }
      ERR_clear_error();
    }
  }

  std::unique_ptr<STACK_OF(X509), decltype(&free_x509_stack_only)> chain(sk_X509_new_null(), free_x509_stack_only);
  if (!chain) {
    if (out_error) *out_error = std::string("internal: failed to allocate x509 chain: ") + openssl_last_error_text();
    return false;
  }
  for (const auto& cert : chain_certs) {
    if (!cert) continue;
    if (sk_X509_push(chain.get(), cert.get()) == 0) {
      if (out_error) *out_error = std::string("internal: failed to append untrusted cert: ") + openssl_last_error_text();
      return false;
    }
  }

  std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(X509_STORE_CTX_new(), X509_STORE_CTX_free);
  if (!ctx) {
    if (out_error) *out_error = std::string("internal: failed to allocate x509 verify context: ") + openssl_last_error_text();
    return false;
  }
  if (X509_STORE_CTX_init(ctx.get(), store.get(), leaf, sk_X509_num(chain.get()) > 0 ? chain.get() : nullptr) != 1) {
    if (out_error) *out_error = std::string("internal: failed to init x509 verify context: ") + openssl_last_error_text();
    return false;
  }

  ERR_clear_error();
  const int ok = X509_verify_cert(ctx.get());
  out_result->verified = (ok == 1);
  out_result->verify_error_code = X509_STORE_CTX_get_error(ctx.get());
  out_result->verify_error_depth = X509_STORE_CTX_get_error_depth(ctx.get());
  if (!out_result->verified) {
    const char* msg = X509_verify_cert_error_string(out_result->verify_error_code);
    out_result->verify_error = msg ? std::string(msg) : std::string("x509_verify_failed");
  }

  std::unique_ptr<STACK_OF(X509), decltype(&free_x509_stack_with_certs)> verified_chain(
    X509_STORE_CTX_get1_chain(ctx.get()),
    free_x509_stack_with_certs);
  if (verified_chain) {
    const int n = sk_X509_num(verified_chain.get());
    for (int i = 0; i < n; i++) {
      X509* cert = sk_X509_value(verified_chain.get(), i);
      if (!cert) continue;
      out_result->chain_subjects.push_back(x509_name_to_string(X509_get_subject_name(cert)));
      out_result->chain_sha256_hex.push_back(x509_sha256_hex(cert));
    }
    if (!out_result->chain_sha256_hex.empty()) {
      const std::string& tail = out_result->chain_sha256_hex.back();
      for (const auto& root : roots) {
        if (root.sha256_hex == tail) out_result->matched_root_kids.push_back(root.kid);
      }
      std::sort(out_result->matched_root_kids.begin(), out_result->matched_root_kids.end());
      out_result->matched_root_kids.erase(
        std::unique(out_result->matched_root_kids.begin(), out_result->matched_root_kids.end()),
        out_result->matched_root_kids.end());
    }
  }
  return true;
}

static bool build_edge_auth_cert_roots_bundle(
  const DaemonConfig& cfg,
  Json::Value* out_bundle,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bundle) return false;

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "edge_auth_cert_roots_v1";
  bundle["created_utc_ms"] = (Json::Int64)now_utc_ms();
  bundle["rotation_epoch"] = (Json::Int64)cfg.edge_auth_cert_roots_epoch;
  bundle["updated_utc_ms"] = (Json::Int64)cfg.edge_auth_cert_roots_updated_utc_ms;
  {
    Json::Value roots(Json::objectValue);
    for (const auto& it : cfg.edge_auth_cert_roots_pem) {
      if (!it.first.empty() && !it.second.empty()) roots[it.first] = it.second;
    }
    bundle["cert_roots_pem"] = roots;
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
  att["schema"] = "edge_auth_cert_roots_attest_v1";
  att["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  att["rotation_epoch"] = (Json::Int64)cfg.edge_auth_cert_roots_epoch;
  att["signing_schema"] = "edge_auth_cert_roots_v1";

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

static bool build_edge_auth_revocations_bundle(
  const DaemonConfig& cfg,
  Json::Value* out_bundle,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bundle) return false;

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "edge_auth_revocations_v1";
  bundle["created_utc_ms"] = (Json::Int64)now_utc_ms();
  bundle["rotation_epoch"] = (Json::Int64)cfg.edge_auth_revocations_epoch;
  bundle["updated_utc_ms"] = (Json::Int64)cfg.edge_auth_revocations_updated_utc_ms;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.edge_auth_revoked_kids) if (!s.empty()) arr.append(s);
    bundle["revoked_kids"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.edge_auth_revoked_node_ids) if (!s.empty()) arr.append(s);
    bundle["revoked_node_ids"] = arr;
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
  att["schema"] = "edge_auth_revocations_attest_v1";
  att["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  att["rotation_epoch"] = (Json::Int64)cfg.edge_auth_revocations_epoch;
  att["signing_schema"] = "edge_auth_revocations_v1";

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

static std::vector<std::string> normalize_edge_member_node_ids(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    const std::string s = trim_copy(raw);
    if (s.empty() || !edge_id_is_safe(s)) continue;
    if (!string_vec_contains(out, s)) out.push_back(s);
  }
  return out;
}

static bool build_edge_consensus_membership_bundle(
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
  bundle["schema"] = "edge_consensus_membership_v1";
  bundle["created_utc_ms"] = (Json::Int64)now_utc_ms();
  bundle["cluster_id"] = cluster_id;
  bundle["membership_epoch"] = (Json::Int64)pol.membership_epoch;
  bundle["updated_utc_ms"] = (Json::Int64)pol.updated_utc_ms;
  bundle["campaign_delay_ms"] = (Json::Int64)pol.campaign_delay_ms;
  bundle["campaign_retry_ms"] = (Json::Int64)pol.campaign_retry_ms;
  bundle["campaign_retry_max_ms"] = (Json::Int64)pol.campaign_retry_max_ms;
  bundle["campaign_retry_backoff_factor"] = (Json::Int64)pol.campaign_retry_backoff_factor;
  bundle["leader_heartbeat_ms"] = (Json::Int64)pol.leader_heartbeat_ms;
  bundle["leader_lease_ms"] = (Json::Int64)pol.leader_lease_ms;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& member : pol.member_node_ids) arr.append(member);
    bundle["member_node_ids"] = arr;
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
  att["schema"] = "edge_consensus_membership_attest_v1";
  att["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  att["cluster_id"] = cluster_id;
  att["membership_epoch"] = (Json::Int64)pol.membership_epoch;
  att["signing_schema"] = "edge_consensus_membership_v1";

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

static bool enqueue_edge_platform_bundle(
  AgentDb* db_or_null,
  const std::string& target_node_id,
  const std::string& msg_type,
  const char* body_field,
  const Json::Value& bundle,
  const std::map<std::string, std::string>* confidentiality_keys_or_null,
  const std::string& confidential_kid,
  int64_t* out_outbox_id,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_outbox_id) *out_outbox_id = 0;
  if (!db_or_null || !db_or_null->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id)) {
    if (out_error) *out_error = "missing/invalid target_node_id";
    return false;
  }
  if (msg_type.empty() || !body_field || std::string(body_field).empty()) {
    if (out_error) *out_error = "internal error";
    return false;
  }

  AgentDb::EdgeNodeRow target_row;
  std::string terr;
  if (!db_or_null->get_edge_node(target_node_id, &target_row, &terr)) {
    if (out_error) *out_error = "target node not found";
    return false;
  }

  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  env["type"] = msg_type;
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(target_node_id);
  Json::Value body(Json::objectValue);
  body[body_field] = bundle;
  env["body"] = body;
  if (!confidential_kid.empty()) {
    std::string ecode;
    std::string eerr;
    const std::map<std::string, std::string> empty_keys;
    if (!edge_confidentiality_wrap_envelope_body(
          &env,
          confidentiality_keys_or_null ? *confidentiality_keys_or_null : empty_keys,
          confidential_kid,
          &ecode,
          &eerr)) {
      if (out_error) *out_error = eerr.empty() ? ecode : eerr;
      return false;
    }
  }

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = target_node_id;
  orow.ts_utc_ms = env["ts_utc_ms"].asInt64();
  orow.envelope_json = edge_json_stringify_compact(env);
  std::string oerr;
  if (!db_or_null->insert_edge_outbox_message(orow, out_outbox_id, &oerr)) {
    if (out_error) *out_error = oerr.empty() ? "failed to enqueue bundle" : oerr;
    return false;
  }
  return true;
}

}  // namespace

bool verify_edge_cert_chain_against_roots(
  const DaemonConfig& cfg,
  const std::string& cert_pem,
  const std::vector<std::string>& untrusted_cert_pem,
  EdgeVerifyChainResult* out_result,
  std::string* out_error
) {
  return verify_edge_cert_chain_against_roots_impl(cfg, cert_pem, untrusted_cert_pem, out_result, out_error);
}

void handle_config_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["service"] = "agentd";
  out["version"] = "0.1";
  out["have_jsoncpp"] = true;
#if defined(AGENT_HAVE_SQLITE3)
  out["have_sqlite3"] = true;
#else
  out["have_sqlite3"] = false;
#endif

  Json::Value daemon(Json::objectValue);
  daemon["listen_host"] = cfg.listen_host;
  daemon["listen_port"] = (Json::UInt64)cfg.listen_port;
  daemon["base_url"] = cfg.base_url;
  daemon["model"] = cfg.model;
  daemon["summary_model"] = cfg.summary_model.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.summary_model);
  daemon["summary_max_chars"] = (Json::UInt64)cfg.summary_max_chars;
  daemon["timeout_ms"] = (Json::Int64)cfg.timeout_ms;
  daemon["proxy_url_set"] = !cfg.proxy_url.empty();
  daemon["api_key_set"] = !cfg.api_key.empty();
  {
    Json::Value keys(Json::objectValue);
    auto has_key = [&](const char* p) -> bool {
      const auto it = cfg.provider_keys.find(p ? p : "");
      return it != cfg.provider_keys.end() && !it->second.empty();
    };
    keys["deepseek"] = has_key("deepseek");
    keys["openrouter"] = has_key("openrouter");
    keys["moonshot"] = has_key("moonshot");
    keys["openai"] = has_key("openai");
    daemon["provider_keys_set"] = keys;
  }
  daemon["auth_enabled"] = !cfg.auth_token.empty();
  daemon["auth_cookie_name"] = cfg.auth_cookie_name;
  daemon["allow_unauthenticated_non_loopback"] = cfg.allow_unauthenticated_non_loopback;
  daemon["db_path"] = cfg.db_path.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.db_path);
  daemon["state_dir"] = cfg.state_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.state_dir);
  daemon["sessions_root_dir"] = cfg.sessions_root_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.sessions_root_dir);
  {
    daemon["audio_webrtc"] = config_audio_webrtc_metadata_json(cfg);
  }
  daemon["upload_max_bytes"] = (Json::UInt64)cfg.upload_max_bytes;
  {
    Json::Value bs(Json::objectValue);
    bs["mode"] = cfg.blob_store_mode;
    bs["endpoint"] = cfg.blob_store_endpoint.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.blob_store_endpoint);
    bs["region"] = cfg.blob_store_region;
    bs["bucket"] = cfg.blob_store_bucket.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.blob_store_bucket);
    bs["prefix"] = cfg.blob_store_prefix;
    bs["path_style"] = cfg.blob_store_path_style;
    bs["read_mode"] = cfg.blob_store_read_mode;
    bs["cache_mode"] = cfg.blob_store_cache_mode;
    bs["cache_max_bytes"] = (Json::UInt64)cfg.blob_store_cache_max_bytes;
    bs["presign_ttl_sec"] = (Json::Int64)cfg.blob_store_presign_ttl_sec;
    bs["timeout_ms"] = (Json::Int64)cfg.blob_store_timeout_ms;
    bs["access_key_set"] = !cfg.blob_store_access_key.empty();
    bs["secret_key_set"] = !cfg.blob_store_secret_key.empty();
    bs["session_token_set"] = !cfg.blob_store_session_token.empty();
    daemon["blob_store"] = bs;
  }
  {
    Json::Value bt(Json::objectValue);
    bt["local_max_bytes"] = (Json::Int64)cfg.blob_tier_local_max_bytes;
    bt["local_max_age_ms"] = (Json::Int64)cfg.blob_tier_local_max_age_ms;
    bt["promote_after_ms"] = (Json::Int64)cfg.blob_tier_promote_after_ms;
    bt["promote_max_bytes"] = (Json::Int64)cfg.blob_tier_promote_max_bytes;
    daemon["blob_tier"] = bt;
  }
  daemon["max_steps_default"] = (Json::UInt64)cfg.max_steps_default;
  daemon["max_tool_calls_total_default"] = (Json::UInt64)cfg.max_tool_calls_total_default;
  daemon["max_tool_calls_per_tool_default"] = (Json::UInt64)cfg.max_tool_calls_per_tool_default;
  daemon["max_tool_call_args_chars_default"] = (Json::UInt64)cfg.max_tool_call_args_chars_default;
  daemon["max_tool_result_chars_default"] = (Json::UInt64)cfg.max_tool_result_chars_default;
  {
    Json::Value ota(Json::objectValue);
    ota["enabled"] = cfg.ota_enable;
    ota["command_configured"] = !cfg.ota_command.empty();
    ota["command_timeout_ms"] = (Json::Int64)cfg.ota_command_timeout_ms;
    ota["drain_timeout_ms"] = (Json::Int64)cfg.ota_drain_timeout_ms;
    daemon["ota"] = ota;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : cfg.tool_call_limits_default) {
      Json::Value o(Json::objectValue);
      o["tool"] = p.first;
      o["max_calls"] = (Json::UInt64)p.second;
      arr.append(o);
    }
    daemon["tool_call_limits_default"] = arr;
  }
  out["daemon"] = daemon;

  Json::Value cors(Json::objectValue);
  cors["enabled"] = !cors_cfg.origins.empty() || !cors_cfg.routes.empty();
  Json::Value origins(Json::arrayValue);
  for (const auto& o : cors_cfg.origins) origins.append(o);
  cors["origins"] = origins;
  cors["allow_headers"] = cors_cfg.allow_headers;
  cors["allow_methods"] = cors_cfg.allow_methods;
  cors["allow_credentials"] = cors_cfg.allow_credentials;
  cors["max_age_seconds"] = cors_cfg.max_age_seconds;
  if (!cors_cfg.routes.empty()) {
    Json::Value routes(Json::arrayValue);
    for (const auto& r : cors_cfg.routes) {
      Json::Value o(Json::objectValue);
      o["path_prefix"] = r.path_prefix;
      Json::Value ro(Json::arrayValue);
      for (const auto& origin : r.origins) ro.append(origin);
      o["origins"] = ro;
      routes.append(o);
    }
    cors["routes"] = routes;
  }
  out["cors"] = cors;

  Json::Value sandbox(Json::objectValue);
  sandbox["tools"] = cfg.tools;
  sandbox["yolo_default"] = cfg.yolo_default;
  sandbox["host_policy"] = host_policy_to_string(cfg.host_policy);
  sandbox["system_profile"] = cfg.system_profile;
  {
    const auto allow = mount_allowlist_status();
    Json::Value allow_json(Json::objectValue);
    allow_json["path"] = allow.path;
    allow_json["present"] = allow.present;
    allow_json["loaded"] = allow.loaded;
    allow_json["allowed_roots"] = Json::UInt64(allow.allowed_roots);
    allow_json["blocked_patterns"] = Json::UInt64(allow.blocked_patterns);
    if (!allow.error.empty()) allow_json["error"] = allow.error;
    sandbox["mount_allowlist"] = allow_json;
  }
  out["sandbox"] = sandbox;

  Json::Value policy(Json::objectValue);
  policy["mode"] = cfg.policy_mode;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.policy_tool_allowlist) if (!s.empty()) arr.append(s);
    policy["tool_allowlist"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.policy_tool_denylist) if (!s.empty()) arr.append(s);
    policy["tool_denylist"] = arr;
  }
  policy["max_steps"] = (Json::UInt64)cfg.policy_max_steps;
  policy["max_tool_calls_total"] = (Json::UInt64)cfg.policy_max_tool_calls_total;
  policy["max_tool_calls_per_tool"] = (Json::UInt64)cfg.policy_max_tool_calls_per_tool;
  policy["max_tool_call_args_chars"] = (Json::UInt64)cfg.policy_max_tool_call_args_chars;
  policy["max_tool_result_chars"] = (Json::UInt64)cfg.policy_max_tool_result_chars;
  if (!cfg.policy_approval_tools.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.policy_approval_tools) if (!s.empty()) arr.append(s);
    policy["approval_tools"] = arr;
  }
  policy["approval_required"] = cfg.policy_approval_required;
  if (!cfg.policy_approval_roles.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.policy_approval_roles) if (!s.empty()) arr.append(s);
    policy["approval_roles"] = arr;
  }
  policy["approval_timeout_ms"] = (Json::Int64)cfg.policy_approval_timeout_ms;
  policy["approval_poll_ms"] = (Json::Int64)cfg.policy_approval_poll_ms;
  out["policy"] = policy;

  Json::Value jobs(Json::objectValue);
  jobs["job_ttl_ms"] = (Json::Int64)cfg.job_ttl_ms;
  jobs["max_jobs"] = (Json::UInt64)cfg.max_jobs;
  out["jobs"] = jobs;

  Json::Value engines(Json::objectValue);
  engines["job_max_concurrency"] = cfg.job_engine_max_concurrency;
  engines["job_poll_ms"] = cfg.job_engine_poll_ms;
  engines["workflow_max_concurrency"] = cfg.workflow_engine_max_concurrency;
  engines["workflow_poll_ms"] = cfg.workflow_engine_poll_ms;
  engines["workflow_max_inflight_per_workflow"] = cfg.workflow_engine_max_inflight_per_workflow;
  engines["workflow_max_inflight_per_session"] = cfg.workflow_engine_max_inflight_per_session;
  engines["workflow_fair_queue_policy"] = cfg.workflow_engine_fair_queue_policy;
  engines["workflow_fair_queue_max_session_weight"] = cfg.workflow_engine_fair_queue_max_session_weight;
  engines["workflow_fair_queue_max_schedule_len"] = cfg.workflow_engine_fair_queue_max_schedule_len;
  engines["workflow_drr_cost_model"] = cfg.workflow_engine_drr_cost_model;
  engines["workflow_admit_max_inflight_tasks_per_session"] = cfg.workflow_admit_max_inflight_tasks_per_session;
  engines["workflow_admit_max_inflight_tasks_total"] = cfg.workflow_admit_max_inflight_tasks_total;
  engines["workflow_enable_http_tasks"] = cfg.workflow_enable_http_tasks;
  if (!cfg.workflow_http_allow_hosts.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& h : cfg.workflow_http_allow_hosts) {
      if (!h.empty()) arr.append(h);
    }
    engines["workflow_http_allow_hosts"] = arr;
  }
  if (!cfg.workflow_http_allow_cidrs.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& c : cfg.workflow_http_allow_cidrs) {
      if (!c.empty()) arr.append(c);
    }
    engines["workflow_http_allow_cidrs"] = arr;
  }
  engines["workflow_http_deny_private_addrs"] = cfg.workflow_http_deny_private_addrs;
  if (!cfg.workflow_http_deny_cidrs.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& c : cfg.workflow_http_deny_cidrs) {
      if (!c.empty()) arr.append(c);
    }
    engines["workflow_http_deny_cidrs"] = arr;
  }
  engines["workflow_http_dns_pin"] = cfg.workflow_http_dns_pin;
  out["engines"] = engines;

  Json::Value edge_auth(Json::objectValue);
  edge_auth["required"] = cfg.edge_auth_required;
  edge_auth["require_ts"] = cfg.edge_auth_require_ts;
  edge_auth["max_skew_ms"] = (Json::Int64)cfg.edge_auth_max_skew_ms;
  edge_auth["require_seq"] = cfg.edge_auth_require_seq;
  edge_auth["kid_policy"] = cfg.edge_auth_kid_policy;
  edge_auth["hmac_keys_set"] = (Json::UInt64)cfg.edge_auth_hmac_keys.size();
  edge_auth["ed25519_pubkeys_set"] = (Json::UInt64)cfg.edge_auth_ed25519_pubkeys.size();
  edge_auth["trust_roots_epoch"] = (Json::Int64)cfg.edge_auth_trust_roots_epoch;
  edge_auth["trust_roots_updated_utc_ms"] = (Json::Int64)cfg.edge_auth_trust_roots_updated_utc_ms;
  edge_auth["cert_roots_set"] = (Json::UInt64)cfg.edge_auth_cert_roots_pem.size();
  edge_auth["cert_roots_epoch"] = (Json::Int64)cfg.edge_auth_cert_roots_epoch;
  edge_auth["cert_roots_updated_utc_ms"] = (Json::Int64)cfg.edge_auth_cert_roots_updated_utc_ms;
  edge_auth["require_manifest_cert_chain"] = cfg.edge_auth_require_manifest_cert_chain;
  edge_auth["revoked_kids_set"] = (Json::UInt64)cfg.edge_auth_revoked_kids.size();
  edge_auth["revoked_node_ids_set"] = (Json::UInt64)cfg.edge_auth_revoked_node_ids.size();
  edge_auth["revocations_epoch"] = (Json::Int64)cfg.edge_auth_revocations_epoch;
  edge_auth["revocations_updated_utc_ms"] = (Json::Int64)cfg.edge_auth_revocations_updated_utc_ms;
  out["edge_auth"] = edge_auth;

  Json::Value edge_confidentiality(Json::objectValue);
  edge_confidentiality["required"] = cfg.edge_confidentiality_required;
  edge_confidentiality["keys_set"] = (Json::UInt64)cfg.edge_confidentiality_keys.size();
  out["edge_confidentiality"] = edge_confidentiality;

  Json::Value edge_consensus(Json::objectValue);
  {
    Json::Value meta = config_edge_consensus_metadata_json(cfg);
    for (const auto& key : meta.getMemberNames()) edge_consensus[key] = meta[key];
  }
  edge_consensus["clusters_set"] = (Json::UInt64)cfg.edge_consensus_clusters.size();
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& it : cfg.edge_consensus_clusters) {
      if (!it.first.empty() && !it.second.member_node_ids.empty()) arr.append(it.first);
    }
    edge_consensus["cluster_ids"] = arr;
  }
  out["edge_consensus"] = edge_consensus;

  Json::Value edge_attest(Json::objectValue);
  edge_attest["required"] = cfg.edge_attest_required;
  edge_attest["require_sig"] = cfg.edge_attest_require_sig;
  out["edge_attest"] = edge_attest;

  Json::Value memory(Json::objectValue);
  memory["consolidate_interval_ms"] = (Json::Int64)cfg.memory_consolidate_interval_ms;
  memory["consolidate_daily_days"] = cfg.memory_consolidate_daily_days;
  memory["consolidate_keep_checkpoints"] = cfg.memory_consolidate_keep_checkpoints;
  memory["recap_daily_interval_ms"] = (Json::Int64)cfg.memory_recap_daily_interval_ms;
  memory["recap_weekly_interval_ms"] = (Json::Int64)cfg.memory_recap_weekly_interval_ms;
  memory["recap_daily_days"] = cfg.memory_recap_daily_days;
  memory["recap_weekly_days"] = cfg.memory_recap_weekly_days;
  memory["retention_interval_ms"] = (Json::Int64)cfg.memory_retention_interval_ms;
  memory["retention_daily_max_days"] = cfg.memory_retention_daily_max_days;
  memory["retention_daily_max_bytes"] = (Json::Int64)cfg.memory_retention_daily_max_bytes;
  memory["retention_checkpoint_max_days"] = cfg.memory_retention_checkpoint_max_days;
  memory["retention_checkpoint_max_count"] = cfg.memory_retention_checkpoint_max_count;
  memory["retention_structured_deprecate_days"] = cfg.memory_retention_structured_deprecate_days;
  memory["retention_structured_deprecate_max_entries"] = cfg.memory_retention_structured_deprecate_max_entries;
  memory["salience_daily_days"] = cfg.memory_salience_daily_days;
  memory["salience_max_items"] = cfg.memory_salience_max_items;
  memory["salience_structured_max_items"] = cfg.memory_salience_structured_max_items;
  memory["salience_daily_max_items"] = cfg.memory_salience_daily_max_items;
  memory["salience_half_life_days"] = cfg.memory_salience_half_life_days;
  memory["salience_importance_weight"] = cfg.memory_salience_importance_weight;
  out["memory"] = memory;

  resp->body = json_stringify(out);
  return;
}

void handle_config_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store) {
    resp->status = 500;
    resp->body = json_error_body("missing cfg_store");
    return;
  }
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }
  const DaemonConfig old_cfg = cfg_store->snapshot();
  if (!daemon_require_auth(old_cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  DaemonConfig next = old_cfg;

  // Apply updates (non-secret defaults).
  if (args.isMember("base_url") && args["base_url"].isString()) {
    next.base_url = args["base_url"].asString();
  }
  if (args.isMember("model") && args["model"].isString()) {
    next.model = args["model"].asString();
  }
  if (args.isMember("summary_model")) {
    if (args["summary_model"].isNull()) next.summary_model.clear();
    else if (args["summary_model"].isString()) next.summary_model = args["summary_model"].asString();
  }
  if (args.isMember("summary_max_chars") && args["summary_max_chars"].isInt64()) {
    const auto n = args["summary_max_chars"].asInt64();
    if (n >= 0) next.summary_max_chars = (size_t)n;
  }
  if (args.isMember("proxy_url")) {
    if (args["proxy_url"].isNull()) next.proxy_url.clear();
    else if (args["proxy_url"].isString()) next.proxy_url = args["proxy_url"].asString();
  }
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const auto n = args["timeout_ms"].asInt64();
    if (n > 0) next.timeout_ms = (long)n;
  }
  uint64_t n_u64 = 0;
  if (json_get_u64_nonneg(args, "max_steps_default", &n_u64)) {
    next.max_steps_default = (size_t)n_u64;
  }
  if (json_get_u64_nonneg(args, "max_tool_calls_total_default", &n_u64)) {
    next.max_tool_calls_total_default = (size_t)n_u64;
  }
  if (json_get_u64_nonneg(args, "max_tool_calls_per_tool_default", &n_u64)) {
    next.max_tool_calls_per_tool_default = (size_t)n_u64;
  }
  if (json_get_u64_nonneg(args, "max_tool_call_args_chars_default", &n_u64)) {
    next.max_tool_call_args_chars_default = (size_t)n_u64;
  }
  if (json_get_u64_nonneg(args, "max_tool_result_chars_default", &n_u64)) {
    next.max_tool_result_chars_default = (size_t)n_u64;
  }
  if (args.isMember("policy") && args["policy"].isObject()) {
    const Json::Value& pol = args["policy"];
    if (pol.isMember("mode") && pol["mode"].isString()) {
      PolicyMode pm = PolicyMode::Off;
      if (policy_mode_from_string(pol["mode"].asString(), &pm)) {
        next.policy_mode = policy_mode_to_string(pm);
      }
    }
    if (pol.isMember("tool_allowlist") && pol["tool_allowlist"].isArray()) {
      next.policy_tool_allowlist.clear();
      for (const auto& item : pol["tool_allowlist"]) {
        if (!item.isString()) continue;
        std::string s = trim_copy(item.asString());
        if (is_safe_tool_name(s)) next.policy_tool_allowlist.push_back(std::move(s));
      }
    }
    if (pol.isMember("tool_denylist") && pol["tool_denylist"].isArray()) {
      next.policy_tool_denylist.clear();
      for (const auto& item : pol["tool_denylist"]) {
        if (!item.isString()) continue;
        std::string s = trim_copy(item.asString());
        if (is_safe_tool_name(s)) next.policy_tool_denylist.push_back(std::move(s));
      }
    }
    if (json_get_u64_nonneg(pol, "max_steps", &n_u64)) {
      next.policy_max_steps = (size_t)n_u64;
    }
    if (json_get_u64_nonneg(pol, "max_tool_calls_total", &n_u64)) {
      next.policy_max_tool_calls_total = (size_t)n_u64;
    }
    if (json_get_u64_nonneg(pol, "max_tool_calls_per_tool", &n_u64)) {
      next.policy_max_tool_calls_per_tool = (size_t)n_u64;
    }
    if (json_get_u64_nonneg(pol, "max_tool_call_args_chars", &n_u64)) {
      next.policy_max_tool_call_args_chars = (size_t)n_u64;
    }
    if (json_get_u64_nonneg(pol, "max_tool_result_chars", &n_u64)) {
      next.policy_max_tool_result_chars = (size_t)n_u64;
    }
    if (pol.isMember("approval_tools") && pol["approval_tools"].isArray()) {
      next.policy_approval_tools.clear();
      for (const auto& item : pol["approval_tools"]) {
        if (!item.isString()) continue;
        std::string s = trim_copy(item.asString());
        if (is_safe_tool_name(s)) next.policy_approval_tools.push_back(std::move(s));
      }
    }
    if (pol.isMember("approval_required") && (pol["approval_required"].isInt64() || pol["approval_required"].isInt())) {
      const int64_t v = pol["approval_required"].asInt64();
      next.policy_approval_required = (int)std::max<int64_t>(0, v);
    }
    if (pol.isMember("approval_roles") && pol["approval_roles"].isArray()) {
      next.policy_approval_roles.clear();
      for (const auto& item : pol["approval_roles"]) {
        if (!item.isString()) continue;
        std::string s = trim_copy(item.asString());
        if (!s.empty()) next.policy_approval_roles.push_back(std::move(s));
      }
    }
    if (json_get_u64_nonneg(pol, "approval_timeout_ms", &n_u64)) {
      next.policy_approval_timeout_ms = (int64_t)n_u64;
    }
    if (json_get_u64_nonneg(pol, "approval_poll_ms", &n_u64)) {
      next.policy_approval_poll_ms = (int64_t)n_u64;
    }
  }
  if (args.isMember("upload_max_bytes") && (args["upload_max_bytes"].isInt64() || args["upload_max_bytes"].isUInt64())) {
    const unsigned long long kMax = 512ull * 1024ull * 1024ull;
    unsigned long long n = 0;
    if (args["upload_max_bytes"].isInt64()) {
      long long v = args["upload_max_bytes"].asInt64();
      if (v < 0) v = 0;
      n = (unsigned long long)v;
    } else {
      n = (unsigned long long)args["upload_max_bytes"].asUInt64();
    }
    if (n > kMax) n = kMax;
    next.upload_max_bytes = (size_t)n;
  }
  if (args.isMember("blob_store") && args["blob_store"].isObject()) {
    const Json::Value& bs = args["blob_store"];
    if (bs.isMember("mode") && bs["mode"].isString()) {
      const std::string s = trim_copy(bs["mode"].asString());
      if (s == "local" || s == "object") next.blob_store_mode = s;
    }
    if (bs.isMember("endpoint")) {
      if (bs["endpoint"].isNull()) next.blob_store_endpoint.clear();
      else if (bs["endpoint"].isString()) next.blob_store_endpoint = bs["endpoint"].asString();
    }
    if (bs.isMember("region") && bs["region"].isString()) {
      next.blob_store_region = bs["region"].asString();
    }
    if (bs.isMember("bucket")) {
      if (bs["bucket"].isNull()) next.blob_store_bucket.clear();
      else if (bs["bucket"].isString()) next.blob_store_bucket = bs["bucket"].asString();
    }
    if (bs.isMember("prefix") && bs["prefix"].isString()) {
      next.blob_store_prefix = bs["prefix"].asString();
    }
    if (bs.isMember("path_style") && bs["path_style"].isBool()) {
      next.blob_store_path_style = bs["path_style"].asBool();
    }
    if (bs.isMember("read_mode") && bs["read_mode"].isString()) {
      const std::string s = trim_copy(bs["read_mode"].asString());
      if (s == "redirect" || s == "proxy") next.blob_store_read_mode = s;
    }
    if (bs.isMember("cache_mode") && bs["cache_mode"].isString()) {
      const std::string s = trim_copy(bs["cache_mode"].asString());
      if (s == "none" || s == "read-through") next.blob_store_cache_mode = s;
    }
    if (bs.isMember("cache_max_bytes") && (bs["cache_max_bytes"].isInt64() || bs["cache_max_bytes"].isUInt64())) {
      const unsigned long long kMax = 512ull * 1024ull * 1024ull;
      unsigned long long n = 0;
      if (bs["cache_max_bytes"].isInt64()) {
        long long v = bs["cache_max_bytes"].asInt64();
        if (v < 0) v = 0;
        n = (unsigned long long)v;
      } else {
        n = (unsigned long long)bs["cache_max_bytes"].asUInt64();
      }
      if (n > kMax) n = kMax;
      next.blob_store_cache_max_bytes = (size_t)n;
    }
    if (bs.isMember("presign_ttl_sec") && (bs["presign_ttl_sec"].isInt64() || bs["presign_ttl_sec"].isUInt64())) {
      const int64_t n = bs["presign_ttl_sec"].isInt64()
        ? bs["presign_ttl_sec"].asInt64()
        : (int64_t)bs["presign_ttl_sec"].asUInt64();
      next.blob_store_presign_ttl_sec = std::max<int64_t>(1, std::min<int64_t>(604800, n));
    }
    if (bs.isMember("timeout_ms") && (bs["timeout_ms"].isInt64() || bs["timeout_ms"].isUInt64())) {
      const int64_t n = bs["timeout_ms"].isInt64()
        ? bs["timeout_ms"].asInt64()
        : (int64_t)bs["timeout_ms"].asUInt64();
      next.blob_store_timeout_ms = std::max<int64_t>(0, std::min<int64_t>(30LL * 60 * 1000, n));
    }
  }
  if (args.isMember("blob_tier") && args["blob_tier"].isObject()) {
    const Json::Value& bt = args["blob_tier"];
    if (bt.isMember("local_max_bytes") && (bt["local_max_bytes"].isInt64() || bt["local_max_bytes"].isUInt64())) {
      const int64_t n = bt["local_max_bytes"].isInt64()
        ? bt["local_max_bytes"].asInt64()
        : (int64_t)bt["local_max_bytes"].asUInt64();
      next.blob_tier_local_max_bytes = std::max<int64_t>(0, n);
    }
    if (bt.isMember("local_max_age_ms") && (bt["local_max_age_ms"].isInt64() || bt["local_max_age_ms"].isUInt64())) {
      const int64_t n = bt["local_max_age_ms"].isInt64()
        ? bt["local_max_age_ms"].asInt64()
        : (int64_t)bt["local_max_age_ms"].asUInt64();
      next.blob_tier_local_max_age_ms = std::max<int64_t>(0, n);
    }
    if (bt.isMember("promote_after_ms") && (bt["promote_after_ms"].isInt64() || bt["promote_after_ms"].isUInt64())) {
      const int64_t n = bt["promote_after_ms"].isInt64()
        ? bt["promote_after_ms"].asInt64()
        : (int64_t)bt["promote_after_ms"].asUInt64();
      next.blob_tier_promote_after_ms = std::max<int64_t>(0, n);
    }
    if (bt.isMember("promote_max_bytes") && (bt["promote_max_bytes"].isInt64() || bt["promote_max_bytes"].isUInt64())) {
      const int64_t n = bt["promote_max_bytes"].isInt64()
        ? bt["promote_max_bytes"].asInt64()
        : (int64_t)bt["promote_max_bytes"].asUInt64();
      next.blob_tier_promote_max_bytes = std::max<int64_t>(0, n);
    }
  }
  if (args.isMember("memory") && args["memory"].isObject()) {
    const Json::Value& mem = args["memory"];
    if (mem.isMember("consolidate_interval_ms") && (mem["consolidate_interval_ms"].isInt64() || mem["consolidate_interval_ms"].isUInt64())) {
      const int64_t n = mem["consolidate_interval_ms"].isInt64()
        ? mem["consolidate_interval_ms"].asInt64()
        : (int64_t)mem["consolidate_interval_ms"].asUInt64();
      next.memory_consolidate_interval_ms = std::max<int64_t>(0, n);
    }
    if (mem.isMember("consolidate_daily_days") && (mem["consolidate_daily_days"].isInt() || mem["consolidate_daily_days"].isUInt())) {
      const int n = mem["consolidate_daily_days"].isInt()
        ? mem["consolidate_daily_days"].asInt()
        : (int)mem["consolidate_daily_days"].asUInt();
      next.memory_consolidate_daily_days = std::max(0, n);
    }
    if (mem.isMember("consolidate_keep_checkpoints") && (mem["consolidate_keep_checkpoints"].isInt() || mem["consolidate_keep_checkpoints"].isUInt())) {
      const int n = mem["consolidate_keep_checkpoints"].isInt()
        ? mem["consolidate_keep_checkpoints"].asInt()
        : (int)mem["consolidate_keep_checkpoints"].asUInt();
      next.memory_consolidate_keep_checkpoints = std::max(1, n);
    }
    if (mem.isMember("recap_daily_interval_ms") && (mem["recap_daily_interval_ms"].isInt64() || mem["recap_daily_interval_ms"].isUInt64())) {
      const int64_t n = mem["recap_daily_interval_ms"].isInt64()
        ? mem["recap_daily_interval_ms"].asInt64()
        : (int64_t)mem["recap_daily_interval_ms"].asUInt64();
      next.memory_recap_daily_interval_ms = std::max<int64_t>(0, n);
    }
    if (mem.isMember("recap_weekly_interval_ms") && (mem["recap_weekly_interval_ms"].isInt64() || mem["recap_weekly_interval_ms"].isUInt64())) {
      const int64_t n = mem["recap_weekly_interval_ms"].isInt64()
        ? mem["recap_weekly_interval_ms"].asInt64()
        : (int64_t)mem["recap_weekly_interval_ms"].asUInt64();
      next.memory_recap_weekly_interval_ms = std::max<int64_t>(0, n);
    }
    if (mem.isMember("recap_daily_days") && (mem["recap_daily_days"].isInt() || mem["recap_daily_days"].isUInt())) {
      const int n = mem["recap_daily_days"].isInt()
        ? mem["recap_daily_days"].asInt()
        : (int)mem["recap_daily_days"].asUInt();
      next.memory_recap_daily_days = std::max(0, n);
    }
    if (mem.isMember("recap_weekly_days") && (mem["recap_weekly_days"].isInt() || mem["recap_weekly_days"].isUInt())) {
      const int n = mem["recap_weekly_days"].isInt()
        ? mem["recap_weekly_days"].asInt()
        : (int)mem["recap_weekly_days"].asUInt();
      next.memory_recap_weekly_days = std::max(0, n);
    }
    if (mem.isMember("retention_interval_ms") && (mem["retention_interval_ms"].isInt64() || mem["retention_interval_ms"].isUInt64())) {
      const int64_t n = mem["retention_interval_ms"].isInt64()
        ? mem["retention_interval_ms"].asInt64()
        : (int64_t)mem["retention_interval_ms"].asUInt64();
      next.memory_retention_interval_ms = std::max<int64_t>(0, n);
    }
    if (mem.isMember("retention_daily_max_days") && (mem["retention_daily_max_days"].isInt() || mem["retention_daily_max_days"].isUInt())) {
      const int n = mem["retention_daily_max_days"].isInt()
        ? mem["retention_daily_max_days"].asInt()
        : (int)mem["retention_daily_max_days"].asUInt();
      next.memory_retention_daily_max_days = std::max(0, n);
    }
    if (mem.isMember("retention_daily_max_bytes") && (mem["retention_daily_max_bytes"].isInt64() || mem["retention_daily_max_bytes"].isUInt64())) {
      const int64_t n = mem["retention_daily_max_bytes"].isInt64()
        ? mem["retention_daily_max_bytes"].asInt64()
        : (int64_t)mem["retention_daily_max_bytes"].asUInt64();
      next.memory_retention_daily_max_bytes = std::max<int64_t>(0, n);
    }
    if (mem.isMember("retention_checkpoint_max_days") && (mem["retention_checkpoint_max_days"].isInt() || mem["retention_checkpoint_max_days"].isUInt())) {
      const int n = mem["retention_checkpoint_max_days"].isInt()
        ? mem["retention_checkpoint_max_days"].asInt()
        : (int)mem["retention_checkpoint_max_days"].asUInt();
      next.memory_retention_checkpoint_max_days = std::max(0, n);
    }
    if (mem.isMember("retention_checkpoint_max_count") && (mem["retention_checkpoint_max_count"].isInt() || mem["retention_checkpoint_max_count"].isUInt())) {
      const int n = mem["retention_checkpoint_max_count"].isInt()
        ? mem["retention_checkpoint_max_count"].asInt()
        : (int)mem["retention_checkpoint_max_count"].asUInt();
      next.memory_retention_checkpoint_max_count = std::max(0, n);
    }
    if (mem.isMember("retention_structured_deprecate_days") &&
        (mem["retention_structured_deprecate_days"].isInt() || mem["retention_structured_deprecate_days"].isUInt())) {
      const int n = mem["retention_structured_deprecate_days"].isInt()
        ? mem["retention_structured_deprecate_days"].asInt()
        : (int)mem["retention_structured_deprecate_days"].asUInt();
      next.memory_retention_structured_deprecate_days = std::max(0, n);
    }
    if (mem.isMember("retention_structured_deprecate_max_entries") &&
        (mem["retention_structured_deprecate_max_entries"].isInt() || mem["retention_structured_deprecate_max_entries"].isUInt())) {
      const int n = mem["retention_structured_deprecate_max_entries"].isInt()
        ? mem["retention_structured_deprecate_max_entries"].asInt()
        : (int)mem["retention_structured_deprecate_max_entries"].asUInt();
      next.memory_retention_structured_deprecate_max_entries = std::max(0, n);
    }
    if (mem.isMember("salience_daily_days") && (mem["salience_daily_days"].isInt() || mem["salience_daily_days"].isUInt())) {
      const int n = mem["salience_daily_days"].isInt()
        ? mem["salience_daily_days"].asInt()
        : (int)mem["salience_daily_days"].asUInt();
      next.memory_salience_daily_days = std::max(0, std::min(31, n));
    }
    if (mem.isMember("salience_max_items") && (mem["salience_max_items"].isInt() || mem["salience_max_items"].isUInt())) {
      const int n = mem["salience_max_items"].isInt()
        ? mem["salience_max_items"].asInt()
        : (int)mem["salience_max_items"].asUInt();
      next.memory_salience_max_items = std::max(1, std::min(200, n));
    }
    if (mem.isMember("salience_structured_max_items") && (mem["salience_structured_max_items"].isInt() || mem["salience_structured_max_items"].isUInt())) {
      const int n = mem["salience_structured_max_items"].isInt()
        ? mem["salience_structured_max_items"].asInt()
        : (int)mem["salience_structured_max_items"].asUInt();
      next.memory_salience_structured_max_items = std::max(0, std::min(200, n));
    }
    if (mem.isMember("salience_daily_max_items") && (mem["salience_daily_max_items"].isInt() || mem["salience_daily_max_items"].isUInt())) {
      const int n = mem["salience_daily_max_items"].isInt()
        ? mem["salience_daily_max_items"].asInt()
        : (int)mem["salience_daily_max_items"].asUInt();
      next.memory_salience_daily_max_items = std::max(0, std::min(200, n));
    }
    if (mem.isMember("salience_half_life_days") && (mem["salience_half_life_days"].isDouble() || mem["salience_half_life_days"].isInt() || mem["salience_half_life_days"].isUInt())) {
      const double v = mem["salience_half_life_days"].asDouble();
      next.memory_salience_half_life_days = v < 0 ? 0 : v;
    }
    if (mem.isMember("salience_importance_weight") && (mem["salience_importance_weight"].isDouble() || mem["salience_importance_weight"].isInt() || mem["salience_importance_weight"].isUInt())) {
      const double v = mem["salience_importance_weight"].asDouble();
      next.memory_salience_importance_weight = v < 0 ? 0 : v;
    }
  }
  if (args.isMember("edge_auth_required") && args["edge_auth_required"].isBool()) {
    next.edge_auth_required = args["edge_auth_required"].asBool();
  }
  if (args.isMember("edge_auth_require_ts") && args["edge_auth_require_ts"].isBool()) {
    next.edge_auth_require_ts = args["edge_auth_require_ts"].asBool();
  }
  if (args.isMember("edge_auth_max_skew_ms") && args["edge_auth_max_skew_ms"].isInt64()) {
    const auto n = args["edge_auth_max_skew_ms"].asInt64();
    next.edge_auth_max_skew_ms = std::max<int64_t>(0, std::min<int64_t>(30LL * 24 * 60 * 60 * 1000, n));
  } else if (args.isMember("edge_auth_max_skew_ms") && args["edge_auth_max_skew_ms"].isUInt64()) {
    const auto n = (int64_t)args["edge_auth_max_skew_ms"].asUInt64();
    next.edge_auth_max_skew_ms = std::max<int64_t>(0, std::min<int64_t>(30LL * 24 * 60 * 60 * 1000, n));
  }
  if (args.isMember("edge_auth_require_seq") && args["edge_auth_require_seq"].isBool()) {
    next.edge_auth_require_seq = args["edge_auth_require_seq"].asBool();
  }
  if (args.isMember("edge_auth_kid_policy") && args["edge_auth_kid_policy"].isString()) {
    const std::string s = trim_copy(args["edge_auth_kid_policy"].asString());
    if (s == "any" || s == "match_node" || s == "node_prefix") next.edge_auth_kid_policy = s;
  }
  if (args.isMember("edge_auth_require_manifest_cert_chain") && args["edge_auth_require_manifest_cert_chain"].isBool()) {
    next.edge_auth_require_manifest_cert_chain = args["edge_auth_require_manifest_cert_chain"].asBool();
  }
  if (args.isMember("edge_confidentiality_required") && args["edge_confidentiality_required"].isBool()) {
    next.edge_confidentiality_required = args["edge_confidentiality_required"].asBool();
  }
  if (args.isMember("edge_auth_trust_roots_epoch")) {
    if (!args["edge_auth_trust_roots_epoch"].isInt64() && !args["edge_auth_trust_roots_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_trust_roots_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next.edge_auth_trust_roots_epoch = args["edge_auth_trust_roots_epoch"].isInt64()
      ? args["edge_auth_trust_roots_epoch"].asInt64()
      : (int64_t)args["edge_auth_trust_roots_epoch"].asUInt64();
    next.edge_auth_trust_roots_updated_utc_ms = now_utc_ms();
  }
  if (args.isMember("edge_attest_required") && args["edge_attest_required"].isBool()) {
    next.edge_attest_required = args["edge_attest_required"].asBool();
  }
  if (args.isMember("edge_attest_require_sig") && args["edge_attest_require_sig"].isBool()) {
    next.edge_attest_require_sig = args["edge_attest_require_sig"].asBool();
  }
  if (args.isMember("workflow_admit_max_inflight_tasks_per_session") && args["workflow_admit_max_inflight_tasks_per_session"].isInt()) {
    const int n = args["workflow_admit_max_inflight_tasks_per_session"].asInt();
    next.workflow_admit_max_inflight_tasks_per_session = std::max(0, std::min(100000, n));
  }
  if (args.isMember("workflow_admit_max_inflight_tasks_total") && args["workflow_admit_max_inflight_tasks_total"].isInt()) {
    const int n = args["workflow_admit_max_inflight_tasks_total"].asInt();
    next.workflow_admit_max_inflight_tasks_total = std::max(0, std::min(1000000, n));
  }
  if (args.isMember("system_profile") && args["system_profile"].isString()) {
    const std::string p = trim_copy(args["system_profile"].asString());
    auto is_valid = [&](const std::string& s) -> bool {
      if (s.empty() || s.size() > 64) return false;
      for (const char c : s) {
        const bool ok =
          (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.';
        if (!ok) return false;
      }
      return true;
    };
    if (is_valid(p)) {
      if (p == "default" || p == "jules_codex") {
        next.system_profile = p;
      }
    }
  }

  if (args.isMember("tool_call_limits_default")) {
    const Json::Value arr = args["tool_call_limits_default"];
    if (!arr.isArray()) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool_call_limits_default must be an array";
      resp->body = json_stringify(o);
      return;
    }
    next.tool_call_limits_default.clear();
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      const Json::Value item = arr[i];
      if (!item.isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "tool_call_limits_default entry must be an object";
        o["index"] = (Json::UInt64)i;
        resp->body = json_stringify(o);
        return;
      }
      if (!item.isMember("tool") || !item["tool"].isString()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "tool_call_limits_default entry missing tool";
        o["index"] = (Json::UInt64)i;
        resp->body = json_stringify(o);
        return;
      }
      const std::string tool = trim_copy(item["tool"].asString());
      if (!validate_tool_name_best_effort(tool)) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid tool name in tool_call_limits_default";
        o["tool"] = tool;
        resp->body = json_stringify(o);
        return;
      }
      uint64_t max_calls_u64 = 0;
      if (!json_get_u64_nonneg(item, "max_calls", &max_calls_u64)) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "tool_call_limits_default entry missing max_calls";
        o["tool"] = tool;
        resp->body = json_stringify(o);
        return;
      }
      upsert_tool_call_limit(&next.tool_call_limits_default, tool, (size_t)max_calls_u64);
    }
  }

  // Workflow outbound HTTP policy knobs (non-secret, runtime-mutable).
  {
    std::vector<std::string> allow_hosts;
    std::vector<std::string> allow_cidrs;
    std::vector<std::string> deny_cidrs;
    std::string verr;
    if (!read_string_array_best_effort(args, "workflow_http_allow_hosts", &allow_hosts, /*max_n=*/128, /*max_len=*/512, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_allow_hosts" : verr;
      resp->body = json_stringify(o);
      return;
    }
    if (!read_string_array_best_effort(args, "workflow_http_allow_cidrs", &allow_cidrs, /*max_n=*/128, /*max_len=*/256, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_allow_cidrs" : verr;
      resp->body = json_stringify(o);
      return;
    }
    if (!read_string_array_best_effort(args, "workflow_http_deny_cidrs", &deny_cidrs, /*max_n=*/128, /*max_len=*/256, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_deny_cidrs" : verr;
      resp->body = json_stringify(o);
      return;
    }

    // If fields are present, validate and apply.
    if (args.isMember("workflow_http_allow_hosts")) {
      for (const auto& h : allow_hosts) {
        if (!validate_hostport_token_best_effort(h)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_allow_hosts entry";
          o["value"] = h;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_allow_hosts = allow_hosts;
    }
    if (args.isMember("workflow_http_allow_cidrs")) {
      for (const auto& c : allow_cidrs) {
        if (!validate_cidr_token_best_effort(c)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_allow_cidrs entry";
          o["value"] = c;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_allow_cidrs = allow_cidrs;
    }
    if (args.isMember("workflow_http_deny_cidrs")) {
      for (const auto& c : deny_cidrs) {
        if (!validate_cidr_token_best_effort(c)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_deny_cidrs entry";
          o["value"] = c;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_deny_cidrs = deny_cidrs;
    }
    if (args.isMember("workflow_http_deny_private_addrs") && args["workflow_http_deny_private_addrs"].isBool()) {
      next.workflow_http_deny_private_addrs = args["workflow_http_deny_private_addrs"].asBool();
    }
    if (args.isMember("workflow_http_dns_pin") && args["workflow_http_dns_pin"].isBool()) {
      next.workflow_http_dns_pin = args["workflow_http_dns_pin"].asBool();
    }
  }

  // Provider keys:
  // - provider_keys: { deepseek:"...", openrouter:"...", openai:"..." }
  // - or (provider + api_key): set a single provider key
  auto set_provider_key = [&](const std::string& provider, const std::string& key) {
    if (provider.empty()) return;
    // Empty string clears.
    if (key.empty()) {
      next.provider_keys.erase(provider);
      return;
    }
    next.provider_keys[provider] = key;
  };
  if (args.isMember("provider_keys") && args["provider_keys"].isObject()) {
    const auto& pk = args["provider_keys"];
    for (const auto& name : pk.getMemberNames()) {
      if (!is_known_provider_name(name)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid provider name in provider_keys";
        o["provider"] = name;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
      const auto& v = pk[name];
      if (!v.isString() && !v.isNull()) continue;
      const std::string provider = name;
      if (!v.isString()) {
        set_provider_key(provider, "");
      } else {
        set_provider_key(provider, v.asString());
      }
    }
  } else {
    std::string provider = provider_from_base_url(next.base_url);
    if (args.isMember("provider") && args["provider"].isString()) {
      provider = trim_copy(args["provider"].asString());
      if (!is_known_provider_name(provider)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid provider name";
        o["provider"] = provider;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (args.isMember("api_key") && args["api_key"].isString()) {
      set_provider_key(provider, args["api_key"].asString());
    }
  }

  // Managed voice/WebRTC defaults:
  // - audio_webrtc: { "broker_url": "...", "broker_token": "...", "peer_tool_path": "...", "default_runtime_kind": "bundled|external", "node_bin": "..." } (null clears)
  if (args.isMember("audio_webrtc")) {
    if (!args["audio_webrtc"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "audio_webrtc must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& aw = args["audio_webrtc"];
    if (aw.isMember("broker_url")) {
      const Json::Value& v = aw["broker_url"];
      if (v.isNull()) {
        next.audio_webrtc_broker_url.clear();
      } else if (v.isString()) {
        next.audio_webrtc_broker_url = trim_copy(v.asString());
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "audio_webrtc.broker_url must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (aw.isMember("broker_token")) {
      const Json::Value& v = aw["broker_token"];
      if (v.isNull()) {
        next.audio_webrtc_broker_token.clear();
      } else if (v.isString()) {
        next.audio_webrtc_broker_token = trim_copy(v.asString());
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "audio_webrtc.broker_token must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (aw.isMember("peer_tool_path")) {
      const Json::Value& v = aw["peer_tool_path"];
      if (v.isNull()) {
        next.audio_webrtc_peer_tool_path.clear();
      } else if (v.isString()) {
        next.audio_webrtc_peer_tool_path = trim_copy(v.asString());
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "audio_webrtc.peer_tool_path must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (aw.isMember("default_runtime_kind")) {
      const Json::Value& v = aw["default_runtime_kind"];
      if (v.isNull()) {
        next.audio_webrtc_default_runtime_kind.clear();
        next.audio_webrtc_default_runtime_kind_from_env = false;
      } else if (v.isString()) {
        const std::string kind = lower_copy(trim_copy(v.asString()));
        if (kind != "bundled" && kind != "external") {
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "audio_webrtc.default_runtime_kind must be bundled, external, or null";
          resp->status = 400;
          resp->body = json_stringify(o);
          return;
        }
        next.audio_webrtc_default_runtime_kind = kind;
        next.audio_webrtc_default_runtime_kind_from_env = false;
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "audio_webrtc.default_runtime_kind must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (aw.isMember("node_bin")) {
      const Json::Value& v = aw["node_bin"];
      if (v.isNull()) {
        next.audio_webrtc_peer_node_bin = "node";
      } else if (v.isString()) {
        next.audio_webrtc_peer_node_bin = trim_copy(v.asString());
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "audio_webrtc.node_bin must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
  }

  // Managed consensus runtime defaults:
  // - edge_consensus: { "node_tool_path": "/abs/path/to/agentd_edge_consensus_node" } (null clears)
  if (args.isMember("edge_consensus")) {
    if (!args["edge_consensus"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_consensus must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ec = args["edge_consensus"];
    if (ec.isMember("node_tool_path")) {
      const Json::Value& v = ec["node_tool_path"];
      if (v.isNull()) {
        next.edge_consensus_node_tool_path.clear();
      } else if (v.isString()) {
        next.edge_consensus_node_tool_path = trim_copy(v.asString());
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_consensus.node_tool_path must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
    if (ec.isMember("default_runtime_kind")) {
      const Json::Value& v = ec["default_runtime_kind"];
      if (v.isNull()) {
        next.edge_consensus_default_runtime_kind.clear();
        next.edge_consensus_default_runtime_kind_from_env = false;
      } else if (v.isString()) {
        const std::string kind = lower_copy(trim_copy(v.asString()));
        if (kind != "builtin" && kind != "external") {
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_consensus.default_runtime_kind must be builtin, external, or null";
          resp->status = 400;
          resp->body = json_stringify(o);
          return;
        }
        next.edge_consensus_default_runtime_kind = kind;
        next.edge_consensus_default_runtime_kind_from_env = false;
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_consensus.default_runtime_kind must be a string or null";
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
  }

  // Edge auth keyring (secrets):
  // - edge_auth_hmac_keys: { "<kid>": "<secret>", ... } (null clears a kid)
  if (args.isMember("edge_auth_hmac_keys")) {
    if (!args["edge_auth_hmac_keys"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_hmac_keys must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ek = args["edge_auth_hmac_keys"];
    for (const auto& kid : ek.getMemberNames()) {
      const Json::Value& v = ek[kid];
      if (v.isNull()) {
        next.edge_auth_hmac_keys.erase(kid);
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) next.edge_auth_hmac_keys.erase(kid);
        else next.edge_auth_hmac_keys[kid] = s;
      }
    }
  }

  // Edge auth pubkeys (not secret, but stored in the runtime secrets blob for uniformity):
  // - edge_auth_ed25519_pubkeys: { "<kid>": "<base64(pubkey32)>", ... } (null clears a kid)
  if (args.isMember("edge_auth_ed25519_pubkeys")) {
    if (!args["edge_auth_ed25519_pubkeys"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_ed25519_pubkeys must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ek = args["edge_auth_ed25519_pubkeys"];
    for (const auto& kid : ek.getMemberNames()) {
      const Json::Value& v = ek[kid];
      if (v.isNull()) {
        next.edge_auth_ed25519_pubkeys.erase(kid);
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) {
          next.edge_auth_ed25519_pubkeys.erase(kid);
        } else {
          if (kid.empty() || kid.size() > 64 || !edge_id_is_safe(kid)) {
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "invalid edge_auth_ed25519_pubkeys kid";
            o["kid"] = kid;
            resp->status = 400;
            resp->body = json_stringify(o);
            return;
          }
          std::string pk_bytes;
          std::string berr;
          if (!base64_decode(s, &pk_bytes, &berr) || pk_bytes.size() != 32) {
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "invalid edge_auth_ed25519_pubkeys value (expected base64 of 32 bytes)";
            o["kid"] = kid;
            resp->status = 400;
            resp->body = json_stringify(o);
            return;
          }
          next.edge_auth_ed25519_pubkeys[kid] = s;
        }
      }
    }
  }

  // Edge confidentiality shared secrets (secrets):
  // - edge_confidentiality_keys: { "<kid>": "<secret>", ... } (null clears a kid)
  if (args.isMember("edge_confidentiality_keys")) {
    if (!args["edge_confidentiality_keys"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_confidentiality_keys must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ek = args["edge_confidentiality_keys"];
    for (const auto& kid : ek.getMemberNames()) {
      const Json::Value& v = ek[kid];
      if (kid.empty() || kid.size() > 64 || !edge_id_is_safe(kid)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid edge_confidentiality_keys kid";
        o["kid"] = kid;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
      if (v.isNull()) {
        next.edge_confidentiality_keys.erase(kid);
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) next.edge_confidentiality_keys.erase(kid);
        else next.edge_confidentiality_keys[kid] = s;
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_confidentiality_keys values must be strings or null";
        o["kid"] = kid;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
    }
  }

  // Blob store credentials (secrets):
  // - blob_store_secrets: { "access_key": "...", "secret_key": "...", "session_token": "..." }
  if (args.isMember("blob_store_secrets")) {
    if (!args["blob_store_secrets"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "blob_store_secrets must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& bs = args["blob_store_secrets"];
    auto apply_secret = [&](const char* key, std::string* target) {
      if (!key || !target) return;
      if (!bs.isMember(key)) return;
      const Json::Value& v = bs[key];
      if (v.isNull()) {
        target->clear();
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) target->clear();
        else *target = s;
      }
    };
    apply_secret("access_key", &next.blob_store_access_key);
    apply_secret("secret_key", &next.blob_store_secret_key);
    apply_secret("session_token", &next.blob_store_session_token);
  }

  // Persist to daemon DB so defaults survive restarts.
  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  std::string serr;
  if (!save_runtime_secrets_best_effort(*db, next, &serr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = serr.empty() ? "failed to persist runtime secrets" : serr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  cfg_store->replace(next);

  // Return a safe snapshot (no secrets).
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["base_url"] = next.base_url;
  o["model"] = next.model;
  o["system_profile"] = next.system_profile;
  o["summary_model"] = next.summary_model.empty() ? Json::Value(Json::nullValue) : Json::Value(next.summary_model);
  o["summary_max_chars"] = (Json::UInt64)next.summary_max_chars;
  o["timeout_ms"] = (Json::Int64)next.timeout_ms;
  o["upload_max_bytes"] = (Json::UInt64)next.upload_max_bytes;
  o["max_steps_default"] = (Json::UInt64)next.max_steps_default;
  o["max_tool_calls_total_default"] = (Json::UInt64)next.max_tool_calls_total_default;
  o["max_tool_calls_per_tool_default"] = (Json::UInt64)next.max_tool_calls_per_tool_default;
  o["max_tool_call_args_chars_default"] = (Json::UInt64)next.max_tool_call_args_chars_default;
  o["max_tool_result_chars_default"] = (Json::UInt64)next.max_tool_result_chars_default;
  {
    Json::Value pol(Json::objectValue);
    pol["mode"] = next.policy_mode;
    {
      Json::Value arr(Json::arrayValue);
      for (const auto& s : next.policy_tool_allowlist) if (!s.empty()) arr.append(s);
      pol["tool_allowlist"] = arr;
    }
    {
      Json::Value arr(Json::arrayValue);
      for (const auto& s : next.policy_tool_denylist) if (!s.empty()) arr.append(s);
      pol["tool_denylist"] = arr;
    }
    pol["max_steps"] = (Json::UInt64)next.policy_max_steps;
    pol["max_tool_calls_total"] = (Json::UInt64)next.policy_max_tool_calls_total;
    pol["max_tool_calls_per_tool"] = (Json::UInt64)next.policy_max_tool_calls_per_tool;
    pol["max_tool_call_args_chars"] = (Json::UInt64)next.policy_max_tool_call_args_chars;
    pol["max_tool_result_chars"] = (Json::UInt64)next.policy_max_tool_result_chars;
    if (!next.policy_approval_tools.empty()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& s : next.policy_approval_tools) if (!s.empty()) arr.append(s);
      pol["approval_tools"] = arr;
    }
    pol["approval_required"] = next.policy_approval_required;
    if (!next.policy_approval_roles.empty()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& s : next.policy_approval_roles) if (!s.empty()) arr.append(s);
      pol["approval_roles"] = arr;
    }
    pol["approval_timeout_ms"] = (Json::Int64)next.policy_approval_timeout_ms;
    pol["approval_poll_ms"] = (Json::Int64)next.policy_approval_poll_ms;
    o["policy"] = pol;
  }
  o["proxy_url_set"] = !next.proxy_url.empty();
  {
    o["audio_webrtc"] = config_audio_webrtc_metadata_json(next);
  }
  {
    o["edge_consensus"] = config_edge_consensus_metadata_json(next);
  }
  {
    Json::Value bs(Json::objectValue);
    bs["mode"] = next.blob_store_mode;
    bs["endpoint"] = next.blob_store_endpoint.empty() ? Json::Value(Json::nullValue) : Json::Value(next.blob_store_endpoint);
    bs["region"] = next.blob_store_region;
    bs["bucket"] = next.blob_store_bucket.empty() ? Json::Value(Json::nullValue) : Json::Value(next.blob_store_bucket);
    bs["prefix"] = next.blob_store_prefix;
    bs["path_style"] = next.blob_store_path_style;
    bs["read_mode"] = next.blob_store_read_mode;
    bs["cache_mode"] = next.blob_store_cache_mode;
    bs["cache_max_bytes"] = (Json::UInt64)next.blob_store_cache_max_bytes;
    bs["presign_ttl_sec"] = (Json::Int64)next.blob_store_presign_ttl_sec;
    bs["timeout_ms"] = (Json::Int64)next.blob_store_timeout_ms;
    bs["access_key_set"] = !next.blob_store_access_key.empty();
    bs["secret_key_set"] = !next.blob_store_secret_key.empty();
    bs["session_token_set"] = !next.blob_store_session_token.empty();
    o["blob_store"] = bs;
  }
  {
    Json::Value bt(Json::objectValue);
    bt["local_max_bytes"] = (Json::Int64)next.blob_tier_local_max_bytes;
    bt["local_max_age_ms"] = (Json::Int64)next.blob_tier_local_max_age_ms;
    bt["promote_after_ms"] = (Json::Int64)next.blob_tier_promote_after_ms;
    bt["promote_max_bytes"] = (Json::Int64)next.blob_tier_promote_max_bytes;
    o["blob_tier"] = bt;
  }
  {
    Json::Value mem(Json::objectValue);
    mem["consolidate_interval_ms"] = (Json::Int64)next.memory_consolidate_interval_ms;
    mem["consolidate_daily_days"] = next.memory_consolidate_daily_days;
    mem["consolidate_keep_checkpoints"] = next.memory_consolidate_keep_checkpoints;
    mem["recap_daily_interval_ms"] = (Json::Int64)next.memory_recap_daily_interval_ms;
    mem["recap_weekly_interval_ms"] = (Json::Int64)next.memory_recap_weekly_interval_ms;
    mem["recap_daily_days"] = next.memory_recap_daily_days;
    mem["recap_weekly_days"] = next.memory_recap_weekly_days;
    mem["retention_interval_ms"] = (Json::Int64)next.memory_retention_interval_ms;
    mem["retention_daily_max_days"] = next.memory_retention_daily_max_days;
    mem["retention_daily_max_bytes"] = (Json::Int64)next.memory_retention_daily_max_bytes;
    mem["retention_checkpoint_max_days"] = next.memory_retention_checkpoint_max_days;
    mem["retention_checkpoint_max_count"] = next.memory_retention_checkpoint_max_count;
    mem["retention_structured_deprecate_days"] = next.memory_retention_structured_deprecate_days;
    mem["retention_structured_deprecate_max_entries"] = next.memory_retention_structured_deprecate_max_entries;
    mem["salience_daily_days"] = next.memory_salience_daily_days;
    mem["salience_max_items"] = next.memory_salience_max_items;
    mem["salience_structured_max_items"] = next.memory_salience_structured_max_items;
    mem["salience_daily_max_items"] = next.memory_salience_daily_max_items;
    mem["salience_half_life_days"] = next.memory_salience_half_life_days;
    mem["salience_importance_weight"] = next.memory_salience_importance_weight;
    o["memory"] = mem;
  }
  o["edge_auth_required"] = next.edge_auth_required;
  o["edge_auth_require_ts"] = next.edge_auth_require_ts;
  o["edge_auth_max_skew_ms"] = (Json::Int64)next.edge_auth_max_skew_ms;
  o["edge_auth_require_seq"] = next.edge_auth_require_seq;
  o["edge_auth_kid_policy"] = next.edge_auth_kid_policy;
  o["edge_auth_trust_roots_epoch"] = (Json::Int64)next.edge_auth_trust_roots_epoch;
  o["edge_auth_trust_roots_updated_utc_ms"] = (Json::Int64)next.edge_auth_trust_roots_updated_utc_ms;
  o["edge_auth_cert_roots_epoch"] = (Json::Int64)next.edge_auth_cert_roots_epoch;
  o["edge_auth_cert_roots_updated_utc_ms"] = (Json::Int64)next.edge_auth_cert_roots_updated_utc_ms;
  o["edge_auth_require_manifest_cert_chain"] = next.edge_auth_require_manifest_cert_chain;
  o["edge_confidentiality_required"] = next.edge_confidentiality_required;
  o["edge_attest_required"] = next.edge_attest_required;
  o["edge_attest_require_sig"] = next.edge_attest_require_sig;
  {
    Json::Value engines(Json::objectValue);
    Json::Value ah(Json::arrayValue);
    for (const auto& s : next.workflow_http_allow_hosts) if (!s.empty()) ah.append(s);
    Json::Value ac(Json::arrayValue);
    for (const auto& s : next.workflow_http_allow_cidrs) if (!s.empty()) ac.append(s);
    Json::Value dc(Json::arrayValue);
    for (const auto& s : next.workflow_http_deny_cidrs) if (!s.empty()) dc.append(s);
    engines["workflow_http_allow_hosts"] = ah;
    engines["workflow_http_allow_cidrs"] = ac;
    engines["workflow_http_deny_cidrs"] = dc;
    engines["workflow_http_deny_private_addrs"] = next.workflow_http_deny_private_addrs;
    engines["workflow_http_dns_pin"] = next.workflow_http_dns_pin;
    o["engines"] = engines;
  }
  {
    Json::Value keys(Json::objectValue);
    auto has_key = [&](const char* p) -> bool {
      const auto it = next.provider_keys.find(p ? p : "");
      return it != next.provider_keys.end() && !it->second.empty();
    };
    keys["deepseek"] = has_key("deepseek");
    keys["openrouter"] = has_key("openrouter");
    keys["moonshot"] = has_key("moonshot");
    keys["openai"] = has_key("openai");
    o["provider_keys_set"] = keys;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : next.tool_call_limits_default) {
      Json::Value item(Json::objectValue);
      item["tool"] = p.first;
      item["max_calls"] = (Json::UInt64)p.second;
      arr.append(item);
    }
    o["tool_call_limits_default"] = arr;
  }
  o["edge_auth_hmac_keys_set"] = (Json::UInt64)next.edge_auth_hmac_keys.size();
  o["edge_auth_ed25519_pubkeys_set"] = (Json::UInt64)next.edge_auth_ed25519_pubkeys.size();
  o["edge_auth_cert_roots_set"] = (Json::UInt64)next.edge_auth_cert_roots_pem.size();
  o["edge_confidentiality_keys_set"] = (Json::UInt64)next.edge_confidentiality_keys.size();
  resp->body = json_stringify(o);
  return;
}

void handle_edge_auth_trust_roots_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_trust_roots_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "trust_roots_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["trust_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_trust_roots_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string target_node_id =
    args.isMember("target_node_id") && args["target_node_id"].isString() ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string confidential_kid =
    args.isMember("confidential_kid") && args["confidential_kid"].isString() ? trim_copy(args["confidential_kid"].asString()) : "";
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid target_node_id");
    return;
  }
  if (!confidential_kid.empty() && (confidential_kid.size() > 64 || !edge_id_is_safe(confidential_kid))) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid confidential_kid");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_trust_roots_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "trust_roots_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  int64_t outbox_id = 0;
  std::string oerr;
  if (!enqueue_edge_platform_bundle(
        db,
        target_node_id,
        "PLATFORM_TRUST_ROOTS_BUNDLE",
        "trust_roots",
        bundle,
        &cfg.edge_confidentiality_keys,
        confidential_kid,
        &outbox_id,
        &oerr)) {
    if (oerr == "db not available") {
      resp->status = 503;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "target node not found") {
      resp->status = 404;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "unknown confidentiality kid") {
      resp->status = 400;
      resp->body = json_error_body(oerr);
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue trust roots bundle" : oerr;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["target_node_id"] = target_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["trust_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_cert_roots_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_cert_roots_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "cert_roots_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cert_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_cert_roots_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string mode = args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "merge";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = cur.edge_auth_cert_roots_epoch + 1;
  if (args.isMember("rotation_epoch")) {
    if (!args["rotation_epoch"].isInt64() && !args["rotation_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "rotation_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["rotation_epoch"].isInt64() ? args["rotation_epoch"].asInt64() : (int64_t)args["rotation_epoch"].asUInt64();
  }
  if (new_epoch <= cur.edge_auth_cert_roots_epoch) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "rotation_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur.edge_auth_cert_roots_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  if (!args.isMember("edge_auth_cert_roots_pem") || !args["edge_auth_cert_roots_pem"].isObject()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "edge_auth_cert_roots_pem must be an object";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DaemonConfig next = cur;
  if (mode == "replace") next.edge_auth_cert_roots_pem.clear();

  const auto& roots = args["edge_auth_cert_roots_pem"];
  for (const auto& kid : roots.getMemberNames()) {
    if (!validate_edge_kid(kid)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid edge_auth_cert_roots_pem kid";
      o["kid"] = kid;
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const Json::Value& v = roots[kid];
    if (v.isNull()) {
      next.edge_auth_cert_roots_pem.erase(kid);
      continue;
    }
    if (!v.isString()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_cert_roots_pem values must be strings or null";
      o["kid"] = kid;
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const std::string pem = trim_copy(v.asString());
    if (pem.empty()) {
      next.edge_auth_cert_roots_pem.erase(kid);
      continue;
    }
    std::string verr;
    if (!validate_edge_pem_cert_blob_best_effort(pem, &verr)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid edge_auth_cert_roots_pem value" : verr;
      o["kid"] = kid;
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next.edge_auth_cert_roots_pem[kid] = pem;
  }

  next.edge_auth_cert_roots_epoch = new_epoch;
  next.edge_auth_cert_roots_updated_utc_ms = now_utc_ms();

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_cert_roots_bundle(next, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "cert_roots_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rotation_epoch"] = (Json::Int64)next.edge_auth_cert_roots_epoch;
  o["updated_utc_ms"] = (Json::Int64)next.edge_auth_cert_roots_updated_utc_ms;
  o["edge_auth_cert_roots_set"] = (Json::UInt64)next.edge_auth_cert_roots_pem.size();
  o["cert_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_cert_roots_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string target_node_id =
    args.isMember("target_node_id") && args["target_node_id"].isString() ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string confidential_kid =
    args.isMember("confidential_kid") && args["confidential_kid"].isString() ? trim_copy(args["confidential_kid"].asString()) : "";
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid target_node_id");
    return;
  }
  if (!confidential_kid.empty() && (confidential_kid.size() > 64 || !edge_id_is_safe(confidential_kid))) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid confidential_kid");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_cert_roots_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "cert_roots_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  int64_t outbox_id = 0;
  std::string oerr;
  if (!enqueue_edge_platform_bundle(
        db,
        target_node_id,
        "PLATFORM_CERT_ROOTS_BUNDLE",
        "cert_roots",
        bundle,
        &cfg.edge_confidentiality_keys,
        confidential_kid,
        &outbox_id,
        &oerr)) {
    if (oerr == "db not available") {
      resp->status = 503;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "target node not found") {
      resp->status = 404;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "unknown confidentiality kid") {
      resp->status = 400;
      resp->body = json_error_body(oerr);
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue cert roots bundle" : oerr;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["target_node_id"] = target_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["cert_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_cert_roots_verify_chain_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args(Json::objectValue);
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string cert_pem =
    args.isMember("cert_pem") && args["cert_pem"].isString() ? args["cert_pem"].asString() : "";
  if (cert_pem.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing cert_pem");
    return;
  }

  std::vector<std::string> untrusted_cert_pem;
  if (args.isMember("untrusted_cert_pem") && !args["untrusted_cert_pem"].isNull()) {
    if (!args["untrusted_cert_pem"].isArray()) {
      resp->status = 400;
      resp->body = json_error_body("untrusted_cert_pem must be an array");
      return;
    }
    for (Json::ArrayIndex i = 0; i < args["untrusted_cert_pem"].size(); i++) {
      if (!args["untrusted_cert_pem"][i].isString()) {
        resp->status = 400;
        resp->body = json_error_body("untrusted_cert_pem entries must be strings");
        return;
      }
      const std::string pem = args["untrusted_cert_pem"][i].asString();
      if (!pem.empty()) untrusted_cert_pem.push_back(pem);
    }
  }

  EdgeVerifyChainResult result;
  std::string verr;
  if (!verify_edge_cert_chain_against_roots(cfg, cert_pem, untrusted_cert_pem, &result, &verr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = verr.empty() ? "failed to verify certificate chain" : verr;
    resp->status = verr.rfind("internal:", 0) == 0 ? 500 : 400;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["verified"] = result.verified;
  o["rotation_epoch"] = (Json::Int64)cfg.edge_auth_cert_roots_epoch;
  o["updated_utc_ms"] = (Json::Int64)cfg.edge_auth_cert_roots_updated_utc_ms;
  o["edge_auth_cert_roots_set"] = (Json::UInt64)cfg.edge_auth_cert_roots_pem.size();
  o["verify_error_code"] = result.verify_error_code;
  o["verify_error_depth"] = result.verify_error_depth;
  o["verify_error"] = result.verify_error;
  o["leaf_subject"] = result.leaf_subject;
  o["leaf_issuer"] = result.leaf_issuer;
  o["leaf_sha256"] = result.leaf_sha256_hex;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.chain_subjects) arr.append(s);
    o["verified_chain_subjects"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.chain_sha256_hex) arr.append(s);
    o["verified_chain_sha256"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : result.matched_root_kids) arr.append(s);
    o["matched_root_kids"] = arr;
  }
  resp->body = json_stringify(o);
}

void handle_edge_auth_trust_roots_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string mode = args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "merge";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = cur.edge_auth_trust_roots_epoch + 1;
  if (args.isMember("rotation_epoch")) {
    if (!args["rotation_epoch"].isInt64() && !args["rotation_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "rotation_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["rotation_epoch"].isInt64() ? args["rotation_epoch"].asInt64() : (int64_t)args["rotation_epoch"].asUInt64();
  }
  if (new_epoch <= cur.edge_auth_trust_roots_epoch) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "rotation_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur.edge_auth_trust_roots_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  DaemonConfig next = cur;
  if (mode == "replace") {
    next.edge_auth_hmac_keys.clear();
    next.edge_auth_ed25519_pubkeys.clear();
  }

  if (args.isMember("edge_auth_hmac_keys")) {
    if (!args["edge_auth_hmac_keys"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_hmac_keys must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ek = args["edge_auth_hmac_keys"];
    for (const auto& kid : ek.getMemberNames()) {
      if (!validate_edge_kid(kid)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid edge_auth_hmac_keys kid";
        o["kid"] = kid;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
      const Json::Value& v = ek[kid];
      if (v.isNull()) {
        next.edge_auth_hmac_keys.erase(kid);
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) next.edge_auth_hmac_keys.erase(kid);
        else next.edge_auth_hmac_keys[kid] = s;
      }
    }
  }

  if (args.isMember("edge_auth_ed25519_pubkeys")) {
    if (!args["edge_auth_ed25519_pubkeys"].isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "edge_auth_ed25519_pubkeys must be an object";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    const auto& ek = args["edge_auth_ed25519_pubkeys"];
    for (const auto& kid : ek.getMemberNames()) {
      if (!validate_edge_kid(kid)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid edge_auth_ed25519_pubkeys kid";
        o["kid"] = kid;
        resp->status = 400;
        resp->body = json_stringify(o);
        return;
      }
      const Json::Value& v = ek[kid];
      if (v.isNull()) {
        next.edge_auth_ed25519_pubkeys.erase(kid);
      } else if (v.isString()) {
        const std::string s = trim_copy(v.asString());
        if (s.empty()) {
          next.edge_auth_ed25519_pubkeys.erase(kid);
        } else {
          std::string pk_bytes;
          std::string berr;
          if (!base64_decode(s, &pk_bytes, &berr) || pk_bytes.size() != 32) {
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "invalid edge_auth_ed25519_pubkeys value (expected base64 of 32 bytes)";
            o["kid"] = kid;
            resp->status = 400;
            resp->body = json_stringify(o);
            return;
          }
          next.edge_auth_ed25519_pubkeys[kid] = s;
        }
      }
    }
  }

  next.edge_auth_trust_roots_epoch = new_epoch;
  next.edge_auth_trust_roots_updated_utc_ms = now_utc_ms();

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  std::string serr;
  if (!save_runtime_secrets_best_effort(*db, next, &serr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = serr.empty() ? "failed to persist runtime secrets" : serr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_trust_roots_bundle(next, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "trust_roots_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rotation_epoch"] = (Json::Int64)next.edge_auth_trust_roots_epoch;
  o["updated_utc_ms"] = (Json::Int64)next.edge_auth_trust_roots_updated_utc_ms;
  o["edge_auth_hmac_keys_set"] = (Json::UInt64)next.edge_auth_hmac_keys.size();
  o["edge_auth_ed25519_pubkeys_set"] = (Json::UInt64)next.edge_auth_ed25519_pubkeys.size();
  o["trust_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_node_binding_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto node_id_q = query_get(req.query, "node_id");
  const std::string node_id = node_id_q ? trim_copy(*node_id_q) : "";
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["binding"] = edge_auth_node_binding_json(cfg, node_id);
  resp->body = json_stringify(o);
}

void handle_edge_auth_provision_node_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string node_id =
    args.isMember("node_id") && args["node_id"].isString() ? trim_copy(args["node_id"].asString()) : "";
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  const std::string mode =
    args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "merge";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = cur.edge_auth_trust_roots_epoch + 1;
  if (args.isMember("rotation_epoch")) {
    if (!args["rotation_epoch"].isInt64() && !args["rotation_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "rotation_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["rotation_epoch"].isInt64() ? args["rotation_epoch"].asInt64() : (int64_t)args["rotation_epoch"].asUInt64();
  }
  if (new_epoch <= cur.edge_auth_trust_roots_epoch) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "rotation_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur.edge_auth_trust_roots_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  DaemonConfig next = cur;
  if (mode == "replace") {
    next.edge_auth_hmac_keys.clear();
    next.edge_auth_ed25519_pubkeys.clear();
  }

  const std::string hmac_kid =
    args.isMember("hmac_kid") && args["hmac_kid"].isString() ? trim_copy(args["hmac_kid"].asString()) : node_id;
  const std::string ed_kid =
    args.isMember("ed25519_kid") && args["ed25519_kid"].isString() ? trim_copy(args["ed25519_kid"].asString()) : node_id;
  if (!hmac_kid.empty() && !validate_edge_kid(hmac_kid)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid hmac_kid";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  if (!ed_kid.empty() && !validate_edge_kid(ed_kid)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid ed25519_kid";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  const std::string hmac_secret =
    args.isMember("hmac_secret") && args["hmac_secret"].isString() ? trim_copy(args["hmac_secret"].asString()) : "";
  const std::string ed_pubkey =
    args.isMember("ed25519_pubkey") && args["ed25519_pubkey"].isString() ? trim_copy(args["ed25519_pubkey"].asString()) : "";
  const bool clear_hmac = args.isMember("clear_hmac") && args["clear_hmac"].isBool() && args["clear_hmac"].asBool();
  const bool clear_ed = args.isMember("clear_ed25519") && args["clear_ed25519"].isBool() && args["clear_ed25519"].asBool();

  if ((!hmac_secret.empty() || clear_hmac) && !edge_kid_policy_allows(cur.edge_auth_kid_policy, node_id, hmac_kid)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "hmac_kid violates current edge_auth_kid_policy";
    o["kid"] = hmac_kid;
    o["kid_policy"] = cur.edge_auth_kid_policy;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }
  if ((!ed_pubkey.empty() || clear_ed) && !edge_kid_policy_allows(cur.edge_auth_kid_policy, node_id, ed_kid)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "ed25519_kid violates current edge_auth_kid_policy";
    o["kid"] = ed_kid;
    o["kid_policy"] = cur.edge_auth_kid_policy;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  if (clear_hmac) next.edge_auth_hmac_keys.erase(hmac_kid);
  else if (!hmac_secret.empty()) next.edge_auth_hmac_keys[hmac_kid] = hmac_secret;

  if (clear_ed) {
    next.edge_auth_ed25519_pubkeys.erase(ed_kid);
  } else if (!ed_pubkey.empty()) {
    std::string pk_bytes;
    std::string berr;
    if (!base64_decode(ed_pubkey, &pk_bytes, &berr) || pk_bytes.size() != 32) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid ed25519_pubkey (expected base64 of 32 bytes)";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next.edge_auth_ed25519_pubkeys[ed_kid] = ed_pubkey;
  }

  next.edge_auth_trust_roots_epoch = new_epoch;
  next.edge_auth_trust_roots_updated_utc_ms = now_utc_ms();

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  std::string serr;
  if (!save_runtime_secrets_best_effort(*db, next, &serr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = serr.empty() ? "failed to persist runtime secrets" : serr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_trust_roots_bundle(next, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "trust_roots_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value provisioned(Json::objectValue);
  provisioned["node_id"] = node_id;
  if (!hmac_secret.empty() || clear_hmac) provisioned["hmac_kid"] = hmac_kid;
  if (!ed_pubkey.empty() || clear_ed) provisioned["ed25519_kid"] = ed_kid;

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rotation_epoch"] = (Json::Int64)next.edge_auth_trust_roots_epoch;
  o["updated_utc_ms"] = (Json::Int64)next.edge_auth_trust_roots_updated_utc_ms;
  o["provisioned"] = provisioned;
  o["binding"] = edge_auth_node_binding_json(next, node_id);
  o["trust_roots"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_revocations_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_revocations_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "revocations_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["revocations"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_revocations_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string target_node_id =
    args.isMember("target_node_id") && args["target_node_id"].isString() ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string confidential_kid =
    args.isMember("confidential_kid") && args["confidential_kid"].isString() ? trim_copy(args["confidential_kid"].asString()) : "";
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid target_node_id");
    return;
  }
  if (!confidential_kid.empty() && (confidential_kid.size() > 64 || !edge_id_is_safe(confidential_kid))) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid confidential_kid");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_revocations_bundle(cfg, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = berr.empty() ? "revocations_unavailable" : berr;
    }
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  int64_t outbox_id = 0;
  std::string oerr;
  if (!enqueue_edge_platform_bundle(
        db,
        target_node_id,
        "PLATFORM_REVOCATIONS_BUNDLE",
        "revocations",
        bundle,
        &cfg.edge_confidentiality_keys,
        confidential_kid,
        &outbox_id,
        &oerr)) {
    if (oerr == "db not available") {
      resp->status = 503;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "target node not found") {
      resp->status = 404;
      resp->body = json_error_body(oerr);
      return;
    }
    if (oerr == "unknown confidentiality kid") {
      resp->status = 400;
      resp->body = json_error_body(oerr);
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue revocations bundle" : oerr;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["target_node_id"] = target_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["revocations"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_auth_revocations_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string mode = args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "merge";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = cur.edge_auth_revocations_epoch + 1;
  if (args.isMember("rotation_epoch")) {
    if (!args["rotation_epoch"].isInt64() && !args["rotation_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "rotation_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["rotation_epoch"].isInt64() ? args["rotation_epoch"].asInt64() : (int64_t)args["rotation_epoch"].asUInt64();
  }
  if (new_epoch <= cur.edge_auth_revocations_epoch) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "rotation_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur.edge_auth_revocations_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  auto parse_safe_id_array = [&](const char* field, std::vector<std::string>* out) -> bool {
    out->clear();
    if (!args.isMember(field)) return true;
    if (!args[field].isArray()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string(field) + " must be an array";
      resp->status = 400;
      resp->body = json_stringify(o);
      return false;
    }
    for (const auto& item : args[field]) {
      if (!item.isString()) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = std::string(field) + " entries must be strings";
        resp->status = 400;
        resp->body = json_stringify(o);
        return false;
      }
      const std::string s = trim_copy(item.asString());
      if (s.empty() || s.size() > 64 || !edge_id_is_safe(s)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = std::string("invalid ") + field + " entry";
        o["value"] = s;
        resp->status = 400;
        resp->body = json_stringify(o);
        return false;
      }
      if (!string_vec_contains(*out, s)) out->push_back(s);
    }
    return true;
  };

  std::vector<std::string> revoked_kids;
  std::vector<std::string> revoked_node_ids;
  if (!parse_safe_id_array("revoked_kids", &revoked_kids)) return;
  if (!parse_safe_id_array("revoked_node_ids", &revoked_node_ids)) return;

  DaemonConfig next = cur;
  if (mode == "replace") {
    next.edge_auth_revoked_kids.clear();
    next.edge_auth_revoked_node_ids.clear();
  }
  for (const auto& kid : revoked_kids) {
    if (!string_vec_contains(next.edge_auth_revoked_kids, kid)) next.edge_auth_revoked_kids.push_back(kid);
  }
  for (const auto& node_id : revoked_node_ids) {
    if (!string_vec_contains(next.edge_auth_revoked_node_ids, node_id)) next.edge_auth_revoked_node_ids.push_back(node_id);
  }
  next.edge_auth_revocations_epoch = new_epoch;
  next.edge_auth_revocations_updated_utc_ms = now_utc_ms();

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_auth_revocations_bundle(next, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "revocations_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rotation_epoch"] = (Json::Int64)next.edge_auth_revocations_epoch;
  o["updated_utc_ms"] = (Json::Int64)next.edge_auth_revocations_updated_utc_ms;
  o["revoked_kids_set"] = (Json::UInt64)next.edge_auth_revoked_kids.size();
  o["revoked_node_ids_set"] = (Json::UInt64)next.edge_auth_revoked_node_ids.size();
  o["revocations"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_consensus_membership_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto cluster_id_q = query_get(req.query, "cluster_id");
  const std::string cluster_id = cluster_id_q ? trim_copy(*cluster_id_q) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(cfg, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
      resp->status = 500;
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
      resp->status = 500;
    } else if (berr == "consensus_membership_unavailable") {
      o["error"] = berr;
      o["cluster_id"] = cluster_id;
      resp->status = 404;
    } else {
      o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
      resp->status = 500;
    }
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_consensus_membership_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string cluster_id = args.isMember("cluster_id") && args["cluster_id"].isString()
    ? trim_copy(args["cluster_id"].asString()) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "replace";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  if (!args.isMember("member_node_ids") || !args["member_node_ids"].isArray()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "member_node_ids must be an array";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  std::vector<std::string> incoming_members;
  for (const auto& item : args["member_node_ids"]) {
    if (!item.isString()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "member_node_ids entries must be strings";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    incoming_members.push_back(item.asString());
  }
  incoming_members = normalize_edge_member_node_ids(incoming_members);
  if (incoming_members.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "member_node_ids must not be empty";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = 0;
  const auto cur_it = cur.edge_consensus_clusters.find(cluster_id);
  const int64_t cur_epoch = cur_it == cur.edge_consensus_clusters.end() ? 0 : cur_it->second.membership_epoch;
  new_epoch = cur_epoch + 1;
  if (args.isMember("membership_epoch")) {
    if (!args["membership_epoch"].isInt64() && !args["membership_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "membership_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["membership_epoch"].isInt64() ? args["membership_epoch"].asInt64() : (int64_t)args["membership_epoch"].asUInt64();
  }
  if (new_epoch <= cur_epoch) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "membership_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  EdgeConsensusClusterPolicy next_pol;
  if (cur_it != cur.edge_consensus_clusters.end() && mode == "merge") next_pol = cur_it->second;
  for (const auto& member : incoming_members) {
    if (!string_vec_contains(next_pol.member_node_ids, member)) next_pol.member_node_ids.push_back(member);
  }
  next_pol.member_node_ids = normalize_edge_member_node_ids(next_pol.member_node_ids);
  if (next_pol.member_node_ids.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "effective member_node_ids must not be empty";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  next_pol.membership_epoch = new_epoch;
  next_pol.updated_utc_ms = now_utc_ms();
  if (args.isMember("campaign_delay_ms")) {
    if (!args["campaign_delay_ms"].isInt64() && !args["campaign_delay_ms"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "campaign_delay_ms must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.campaign_delay_ms = args["campaign_delay_ms"].isInt64()
      ? args["campaign_delay_ms"].asInt64()
      : (int64_t)args["campaign_delay_ms"].asUInt64();
  }
  if (args.isMember("campaign_retry_ms")) {
    if (!args["campaign_retry_ms"].isInt64() && !args["campaign_retry_ms"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "campaign_retry_ms must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.campaign_retry_ms = args["campaign_retry_ms"].isInt64()
      ? args["campaign_retry_ms"].asInt64()
      : (int64_t)args["campaign_retry_ms"].asUInt64();
  }
  if (args.isMember("campaign_retry_max_ms")) {
    if (!args["campaign_retry_max_ms"].isInt64() && !args["campaign_retry_max_ms"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "campaign_retry_max_ms must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.campaign_retry_max_ms = args["campaign_retry_max_ms"].isInt64()
      ? args["campaign_retry_max_ms"].asInt64()
      : (int64_t)args["campaign_retry_max_ms"].asUInt64();
  }
  if (args.isMember("campaign_retry_backoff_factor")) {
    if (!args["campaign_retry_backoff_factor"].isInt64() && !args["campaign_retry_backoff_factor"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "campaign_retry_backoff_factor must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.campaign_retry_backoff_factor = args["campaign_retry_backoff_factor"].isInt64()
      ? args["campaign_retry_backoff_factor"].asInt64()
      : (int64_t)args["campaign_retry_backoff_factor"].asUInt64();
  }
  if (args.isMember("leader_heartbeat_ms")) {
    if (!args["leader_heartbeat_ms"].isInt64() && !args["leader_heartbeat_ms"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "leader_heartbeat_ms must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.leader_heartbeat_ms = args["leader_heartbeat_ms"].isInt64()
      ? args["leader_heartbeat_ms"].asInt64()
      : (int64_t)args["leader_heartbeat_ms"].asUInt64();
  }
  if (args.isMember("leader_lease_ms")) {
    if (!args["leader_lease_ms"].isInt64() && !args["leader_lease_ms"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "leader_lease_ms must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    next_pol.leader_lease_ms = args["leader_lease_ms"].isInt64()
      ? args["leader_lease_ms"].asInt64()
      : (int64_t)args["leader_lease_ms"].asUInt64();
  }
  next_pol.campaign_delay_ms = std::max<int64_t>(0, std::min<int64_t>(next_pol.campaign_delay_ms, 120000));
  next_pol.campaign_retry_ms = std::max<int64_t>(0, std::min<int64_t>(next_pol.campaign_retry_ms, 120000));
  next_pol.campaign_retry_max_ms = std::max<int64_t>(next_pol.campaign_retry_ms, std::min<int64_t>(next_pol.campaign_retry_max_ms, 300000));
  next_pol.campaign_retry_backoff_factor = std::max<int64_t>(1, std::min<int64_t>(next_pol.campaign_retry_backoff_factor, 8));
  next_pol.leader_heartbeat_ms = std::max<int64_t>(0, std::min<int64_t>(next_pol.leader_heartbeat_ms, 120000));
  next_pol.leader_lease_ms = std::max<int64_t>(next_pol.leader_heartbeat_ms, std::min<int64_t>(next_pol.leader_lease_ms, 300000));

  DaemonConfig next = cur;
  next.edge_consensus_clusters[cluster_id] = next_pol;

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(next, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["membership_epoch"] = (Json::Int64)next_pol.membership_epoch;
  o["updated_utc_ms"] = (Json::Int64)next_pol.updated_utc_ms;
  o["campaign_delay_ms"] = (Json::Int64)next_pol.campaign_delay_ms;
  o["campaign_retry_ms"] = (Json::Int64)next_pol.campaign_retry_ms;
  o["campaign_retry_max_ms"] = (Json::Int64)next_pol.campaign_retry_max_ms;
  o["campaign_retry_backoff_factor"] = (Json::Int64)next_pol.campaign_retry_backoff_factor;
  o["leader_heartbeat_ms"] = (Json::Int64)next_pol.leader_heartbeat_ms;
  o["leader_lease_ms"] = (Json::Int64)next_pol.leader_lease_ms;
  o["member_count"] = (Json::UInt64)next_pol.member_node_ids.size();
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_consensus_membership_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
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
    resp->body = json_stringify(o);
    return;
  }
  const std::string cluster_id = args.isMember("cluster_id") && args["cluster_id"].isString()
    ? trim_copy(args["cluster_id"].asString()) : "";
  const std::string target_node_id = args.isMember("target_node_id") && args["target_node_id"].isString()
    ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string confidential_kid = args.isMember("confidential_kid") && args["confidential_kid"].isString()
    ? trim_copy(args["confidential_kid"].asString()) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }
  if (!edge_id_is_safe(target_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid target_node_id");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(cfg, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr == "consensus_membership_unavailable") {
      o["error"] = berr;
      o["cluster_id"] = cluster_id;
      resp->status = 404;
    } else {
      o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
      resp->status = 500;
    }
    resp->body = json_stringify(o);
    return;
  }

  int64_t outbox_id = 0;
  std::string oerr;
  if (!enqueue_edge_platform_bundle(
        db,
        target_node_id,
        "PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE",
        "membership",
        bundle,
        &cfg.edge_confidentiality_keys,
        confidential_kid,
        &outbox_id,
        &oerr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue consensus membership bundle" : oerr;
    resp->status = o["error"].asString() == "target node not found" ? 404 : 400;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["target_node_id"] = target_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

}  // namespace agentd
