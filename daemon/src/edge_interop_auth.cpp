#include "edge_interop_auth.h"

#include "cbor_decode.h"
#include "cbor_encode.h"
#include "daemon_auth.h"
#include "edge_rules.h"
#include "edge_util.h"
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

bool fixed_time_eq32(const uint8_t a[32], const uint8_t b[32]) {
  uint8_t diff = 0;
  for (size_t i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

static std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

std::string umbmp_result_attest_input_v0_1(
  const std::string& task_id,
  const std::string& step_id,
  const std::string& idempotency_key,
  const std::string& result_sha256,
  int64_t ts_utc_ms
) {
  // Versioned, token-safe signing string for MCU/node attestation over the portable result hash surface.
  //
  // This is intentionally line-based so constrained nodes can implement it with sprintf-like code.
  //
  // IMPORTANT: do not change this without bumping the version tag.
  std::string out;
  out.reserve(256);
  out.append("UM_EAIS_RESULT_ATTEST_v0_1\n");
  out.append(task_id);
  out.push_back('\n');
  out.append(step_id);
  out.push_back('\n');
  out.append(idempotency_key);
  out.push_back('\n');
  out.append(result_sha256);
  out.push_back('\n');
  out.append(std::to_string(ts_utc_ms));
  out.push_back('\n');
  return out;
}

static bool auth_input_bytes_for_alg(
  const Json::Value& env,
  const std::string& alg,
  std::string* out_bytes,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_bytes) return false;
  out_bytes->clear();

  // Signature covers the full envelope including `auth` metadata, except `auth.sig` itself.
  // This lets nodes add anti-replay fields (seq) or future metadata without changing the envelope-level fields.
  Json::Value env2 = env;
  if (env2.isMember("auth") && env2["auth"].isObject()) {
    Json::Value auth2 = env2["auth"];
    if (auth2.isMember("sig")) auth2.removeMember("sig");
    env2["auth"] = auth2;
  }

  const std::string a = trim_copy(alg);
  if (a == "hmac-sha256" || a == "HMAC-SHA256" || a == "ed25519" || a == "ED25519") {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string raw = Json::writeString(wb, env2);

    char* canon = nullptr;
    size_t canon_len = 0;
    std::array<char, 256> errbuf{};
    const agent_status_t st =
      agent_json_c14n_canonicalize(raw.data(), raw.size(), &canon, &canon_len, errbuf.data(), errbuf.size());
    if (st != AGENT_OK || !canon) {
      if (out_error) *out_error = std::string("canonicalize failed: ") + (errbuf[0] ? errbuf.data() : "unknown");
      if (canon) agent_free(canon);
      return false;
    }
    out_bytes->assign(canon, canon_len);
    agent_free(canon);
    return true;
  }

  if (a == "hmac-sha256-cbor" || a == "HMAC-SHA256-CBOR" || a == "ed25519-cbor" || a == "ED25519-CBOR") {
    std::string cbor;
    std::string cerr;
    if (!cbor_encode_json_value(env2, &cbor, &cerr)) {
      if (out_error) *out_error = cerr.empty() ? "cbor canonical encode failed" : cerr;
      return false;
    }
    *out_bytes = std::move(cbor);
    return true;
  }

  if (out_error) *out_error = "unsupported alg";
  return false;
}

// Verifies UM-BMP envelope `auth` when required or present.
//
// Semantics:
// - If cfg.edge_auth_required: missing auth => 401, invalid auth => 401.
// - If not required: missing auth => OK (accept), but if auth is present then it must verify.
// - Malformed `auth` object => 400 (envelope malformed).
bool verify_edge_envelope_auth_best_effort(
  const DaemonConfig& cfg,
  const Json::Value& env,
  int64_t now_utc_ms,
  int64_t ts_utc_ms,
  const std::string& from_id,
  const Json::Value& body,
  HttpResponse* resp
) {
  if (!resp) return false;

  const bool has_auth = env.isMember("auth") && !env["auth"].isNull();
  if (!cfg.edge_auth_required && !has_auth) return true;

  if (has_auth || cfg.edge_auth_required) {
    if (cfg.edge_auth_require_ts) {
      if (ts_utc_ms <= 0) {
        resp->status = 401;
        resp->body = "{\"ok\":false,\"error\":\"edge auth requires ts_utc_ms\"}";
        return false;
      }
    }
    if (cfg.edge_auth_max_skew_ms > 0 && ts_utc_ms > 0) {
      const int64_t delta = std::llabs(now_utc_ms - ts_utc_ms);
      if (delta > cfg.edge_auth_max_skew_ms) {
        resp->status = 401;
        resp->body = "{\"ok\":false,\"error\":\"envelope ts_utc_ms outside allowed skew\"}";
        return false;
      }
    }
  }

  if (cfg.edge_auth_required) {
    if (!has_auth) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"missing envelope.auth\"}";
      return false;
    }
    // Stronger identity binding under required mode.
    if (from_id.rfind("node:", 0) != 0 || from_id.size() <= 5) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"edge auth required: envelope.from must be node:<node_id>\"}";
      return false;
    }
    const std::string node_id_from = trim_copy(from_id.substr(5));
    if (node_id_from.empty() || !edge_id_is_safe(node_id_from)) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"edge auth required: invalid node_id in envelope.from\"}";
      return false;
    }
    if (body.isObject() && body.isMember("node_id") && body["node_id"].isString()) {
      const std::string node_id_body = trim_copy(body["node_id"].asString());
      if (!node_id_body.empty() && node_id_body != node_id_from) {
        resp->status = 401;
        resp->body = "{\"ok\":false,\"error\":\"edge auth required: body.node_id must match envelope.from\"}";
        return false;
      }
    }
  }

  const Json::Value& auth = env["auth"];
  if (!auth.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth (expected object)\"}";
    return false;
  }
  if (!auth.isMember("alg") || !auth["alg"].isString()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.alg (expected string)\"}";
    return false;
  }
  if (!auth.isMember("kid") || !auth["kid"].isString()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.kid (expected string)\"}";
    return false;
  }
  if (!auth.isMember("sig") || !auth["sig"].isString()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.sig (expected string)\"}";
    return false;
  }

  const std::string alg = trim_copy(auth["alg"].asString());
  const std::string kid = trim_copy(auth["kid"].asString());
  const std::string sig_b64 = trim_copy(auth["sig"].asString());
  const std::string alg_norm = trim_copy(alg);
  const bool alg_is_hmac = (alg_norm == "hmac-sha256" || alg_norm == "HMAC-SHA256");
  const bool alg_is_hmac_cbor = (alg_norm == "hmac-sha256-cbor" || alg_norm == "HMAC-SHA256-CBOR");
  const bool alg_is_ed25519 = (alg_norm == "ed25519" || alg_norm == "ED25519");
  const bool alg_is_ed25519_cbor = (alg_norm == "ed25519-cbor" || alg_norm == "ED25519-CBOR");
  const bool alg_ok = alg_is_hmac || alg_is_hmac_cbor || alg_is_ed25519 || alg_is_ed25519_cbor;
  if (!alg_ok) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"unsupported envelope.auth.alg\"}";
      return false;
  }
  if (kid.empty() || kid.size() > 64 || !edge_id_is_safe(kid)) {
    resp->status = 401;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.kid\"}";
    return false;
  }

  // Optional per-node key selection policy.
  // This is checked only when we can bind the envelope to a node identity (`from:"node:<node_id>"`).
  if (from_id.rfind("node:", 0) == 0 && from_id.size() > 5) {
    const std::string node_id_from = trim_copy(from_id.substr(5));
    if (!node_id_from.empty() && edge_id_is_safe(node_id_from)) {
      const std::string pol = cfg.edge_auth_kid_policy.empty() ? "any" : cfg.edge_auth_kid_policy;
      if (pol == "match_node") {
        if (kid != node_id_from) {
          resp->status = 401;
          resp->body = "{\"ok\":false,\"error\":\"edge auth kid policy violation\"}";
          return false;
        }
      } else if (pol == "node_prefix") {
        const std::string pref = node_id_from + ":";
        const bool ok = (kid == node_id_from) || (kid.size() > pref.size() && kid.rfind(pref, 0) == 0);
        if (!ok) {
          resp->status = 401;
          resp->body = "{\"ok\":false,\"error\":\"edge auth kid policy violation\"}";
          return false;
        }
      }
    }
  }

  std::string sig_bytes;
  std::string berr;
  const size_t want_sig_bytes = (alg_is_ed25519 || alg_is_ed25519_cbor) ? 64 : 32;
  if (!base64_decode(sig_b64, &sig_bytes, &berr) || sig_bytes.size() != want_sig_bytes) {
    resp->status = 401;
    resp->body = (want_sig_bytes == 64)
      ? "{\"ok\":false,\"error\":\"invalid envelope.auth.sig (expected base64 of 64 bytes)\"}"
      : "{\"ok\":false,\"error\":\"invalid envelope.auth.sig (expected base64 of 32 bytes)\"}";
    return false;
  }

  std::string input_bytes;
  std::string cerr;
  if (!auth_input_bytes_for_alg(env, alg_norm, &input_bytes, &cerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = cerr.empty() ? "failed to compute envelope auth input" : cerr;
    resp->body = edge_json_stringify_compact(o);
    return false;
  }

  if (alg_is_hmac || alg_is_hmac_cbor) {
    const auto it = cfg.edge_auth_hmac_keys.find(kid);
    if (it == cfg.edge_auth_hmac_keys.end() || it->second.empty()) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"unknown envelope.auth.kid\"}";
      return false;
    }
    uint8_t mac[32];
    agent_hmac_sha256(
      it->second.data(),
      it->second.size(),
      input_bytes.data(),
      input_bytes.size(),
      mac
    );
    if (!fixed_time_eq32(mac, (const uint8_t*)sig_bytes.data())) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.sig\"}";
      return false;
    }
  } else {
    const auto it = cfg.edge_auth_ed25519_pubkeys.find(kid);
    if (it == cfg.edge_auth_ed25519_pubkeys.end() || it->second.empty()) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"unknown envelope.auth.kid\"}";
      return false;
    }
    std::string pk_bytes;
    std::string perr;
    if (!base64_decode(it->second, &pk_bytes, &perr) || pk_bytes.size() != 32) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"invalid configured ed25519 pubkey (expected base64 of 32 bytes)\"}";
      return false;
    }
    if (!agent_ed25519_verify(
          input_bytes.data(),
          input_bytes.size(),
          (const uint8_t*)pk_bytes.data(),
          (const uint8_t*)sig_bytes.data())) {
      resp->status = 401;
      resp->body = "{\"ok\":false,\"error\":\"invalid envelope.auth.sig\"}";
      return false;
    }
  }

  return true;
}

}  // namespace agentd
