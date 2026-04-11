#include "config_endpoint.h"

#include "daemon_auth.h"
#include "edge_platform_bundle.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "runtime_config.h"
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentd {

namespace {

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

}  // namespace agentd
