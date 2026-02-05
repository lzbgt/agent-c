#include "agent/ed25519.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include "base64.h"
#include "cbor_encode.h"
#include "json_util.h"

#include <json/json.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string hex_of_bytes(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

static std::string hex_of_bytes(const uint8_t* p, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = p[i];
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
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
  std::array<char, 256> errbuf{};
  const agent_status_t st =
    agent_json_c14n_canonicalize(raw.data(), raw.size(), &canon, &canon_len, errbuf.data(), errbuf.size());
  if (st != AGENT_OK || !canon) {
    if (out_err) *out_err = std::string("canonicalize failed: ") + (errbuf[0] ? errbuf.data() : "unknown");
    if (canon) agent_free(canon);
    return false;
  }
  out->assign(canon, canon_len);
  agent_free(canon);
  return true;
}

static Json::Value env_no_sig_for(const Json::Value& env) {
  Json::Value v = env;
  if (v.isMember("auth") && v["auth"].isObject()) {
    Json::Value a = v["auth"];
    if (a.isMember("sig")) a.removeMember("sig");
    v["auth"] = a;
  }
  return v;
}

int main(void) {
  Json::Value root(Json::objectValue);
  root["version"] = "umbmp_envelope_auth_vectors_v0.4";
  root["notes"] = "Signing input is the envelope with auth.sig removed (auth metadata stays signed). "
                  "Canonical JSON uses agent_json_c14n_v1; canonical CBOR matches daemon/src/cbor_encode.*.";

  Json::Value vecs(Json::arrayValue);

  // Vector 1: NODE_HELLO (minimal, deterministic types).
  {
    Json::Value env(Json::objectValue);
    env["msg_id"] = "00000000-0000-4000-8000-000000000001";
    env["ts_utc_ms"] = (Json::Int64)1700000000123LL;
    env["type"] = "NODE_HELLO";
    env["from"] = "node:vector_node_1";
    env["to"] = "platform";
    Json::Value body(Json::objectValue);
    body["node_id"] = "vector_node_1";
    body["model"] = "esp32sim_stub";
    body["fw_git_sha"] = "deadbeef";
    body["caps_sha256"] = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    env["body"] = body;

    Json::Value auth(Json::objectValue);
    auth["kid"] = "vector_node_1";
    auth["seq"] = (Json::UInt64)1;
    env["auth"] = auth;

    Json::Value v(Json::objectValue);
    v["name"] = "node_hello_minimal";
    v["envelope"] = env;  // includes auth metadata; signature fields shown separately below

    // Canonical JSON bytes.
    std::string canon_json;
    std::string jerr;
    {
      Json::Value e = env;
      e["auth"]["alg"] = "ed25519";
      const Json::Value no_sig = env_no_sig_for(e);
      if (!canonical_json_bytes(no_sig, &canon_json, &jerr)) {
        std::fprintf(stderr, "canonical json failed: %s\n", jerr.c_str());
        return 2;
      }
      v["canon_json_hex"] = hex_of_bytes(canon_json);
    }

    // Canonical CBOR bytes.
    std::string canon_cbor;
    std::string cerr;
    {
      Json::Value e = env;
      e["auth"]["alg"] = "ed25519-cbor";
      const Json::Value no_sig = env_no_sig_for(e);
      if (!agentd::cbor_encode_json_value(no_sig, &canon_cbor, &cerr)) {
        std::fprintf(stderr, "canonical cbor failed: %s\n", cerr.c_str());
        return 2;
      }
      v["canon_cbor_hex"] = hex_of_bytes(canon_cbor);
    }

    // HMAC (JSON + CBOR).
    {
      Json::Value o(Json::objectValue);
      o["kid"] = "vector_node_1";
      o["secret_ascii"] = "test_secret_node_123";
      o["sig_b64_json_alg_hmac_sha256"] = Json::Value(Json::nullValue);
      o["sig_b64_cbor_alg_hmac_sha256_cbor"] = Json::Value(Json::nullValue);

      // JSON HMAC
      {
        Json::Value e = env;
        e["auth"]["alg"] = "hmac-sha256";
        const Json::Value no_sig = env_no_sig_for(e);
        std::string input;
        std::string err;
        if (!canonical_json_bytes(no_sig, &input, &err)) {
          std::fprintf(stderr, "canonical json failed (hmac): %s\n", err.c_str());
          return 2;
        }
        uint8_t mac[32];
        agent_hmac_sha256(
          "test_secret_node_123",
          strlen("test_secret_node_123"),
          input.data(),
          input.size(),
          mac
        );
        o["sig_b64_json_alg_hmac_sha256"] = base64_encode(mac, sizeof(mac));
      }

      // CBOR HMAC
      {
        Json::Value e = env;
        e["auth"]["alg"] = "hmac-sha256-cbor";
        const Json::Value no_sig = env_no_sig_for(e);
        std::string input;
        std::string err;
        if (!agentd::cbor_encode_json_value(no_sig, &input, &err)) {
          std::fprintf(stderr, "canonical cbor failed (hmac): %s\n", err.c_str());
          return 2;
        }
        uint8_t mac[32];
        agent_hmac_sha256(
          "test_secret_node_123",
          strlen("test_secret_node_123"),
          input.data(),
          input.size(),
          mac
        );
        o["sig_b64_cbor_alg_hmac_sha256_cbor"] = base64_encode(mac, sizeof(mac));
      }
      v["hmac"] = o;
    }

    // Ed25519 (JSON + CBOR) using the RFC 8032 vector 1 seed.
    {
      const uint8_t sk_seed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
      };
      uint8_t pk[32];
      agent_ed25519_publickey(sk_seed, pk);

      Json::Value o(Json::objectValue);
      o["kid"] = "vector_node_1";
      o["sk_seed_hex"] = hex_of_bytes(sk_seed, sizeof(sk_seed));
      o["pk_b64"] = base64_encode(pk, sizeof(pk));

      // JSON signature (ed25519)
      {
        Json::Value e = env;
        e["auth"]["alg"] = "ed25519";
        const Json::Value no_sig = env_no_sig_for(e);
        std::string input;
        std::string err;
        if (!canonical_json_bytes(no_sig, &input, &err)) {
          std::fprintf(stderr, "canonical json failed (ed25519): %s\n", err.c_str());
          return 2;
        }
        uint8_t sig[64];
        agent_ed25519_sign(input.data(), input.size(), sk_seed, pk, sig);
        o["sig_b64_json_alg_ed25519"] = base64_encode(sig, sizeof(sig));
      }

      // CBOR signature (ed25519-cbor)
      {
        Json::Value e = env;
        e["auth"]["alg"] = "ed25519-cbor";
        const Json::Value no_sig = env_no_sig_for(e);
        std::string input;
        std::string err;
        if (!agentd::cbor_encode_json_value(no_sig, &input, &err)) {
          std::fprintf(stderr, "canonical cbor failed (ed25519): %s\n", err.c_str());
          return 2;
        }
        uint8_t sig[64];
        agent_ed25519_sign(input.data(), input.size(), sk_seed, pk, sig);
        o["sig_b64_cbor_alg_ed25519_cbor"] = base64_encode(sig, sizeof(sig));
      }

      v["ed25519"] = o;
    }

    vecs.append(v);
  }

  root["vectors"] = vecs;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  std::printf("%s\n", Json::writeString(wb, root).c_str());
  return 0;
}
