#include "agent/ed25519.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include "base64.h"
#include "cbor_encode.h"
#include "json_util.h"

#include <json/json.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static std::string bytes_from_hex(const std::string& hex) {
  assert(hex.size() % 2 == 0);
  std::string out;
  out.resize(hex.size() / 2);
  for (size_t i = 0; i < out.size(); i++) {
    const int hi = hex_val(hex[2 * i]);
    const int lo = hex_val(hex[2 * i + 1]);
    assert(hi >= 0 && lo >= 0);
    out[i] = (char)((hi << 4) | lo);
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

static void test_vectors_file(const std::string& path) {
  std::string raw;
  {
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    assert(n >= 0);
    raw.resize((size_t)n);
    in.seekg(0, std::ios::beg);
    if (!raw.empty()) in.read(&raw[0], (std::streamsize)raw.size());
    assert(in.good() || in.eof());
  }
  std::string err;
  Json::Value root;
  assert(agentd::json_parse_object(raw, &root, &err));
  assert(root.isMember("vectors") && root["vectors"].isArray());

  const Json::Value vecs = root["vectors"];
  assert(vecs.size() > 0);
  for (Json::ArrayIndex i = 0; i < vecs.size(); i++) {
    const Json::Value v = vecs[i];
    assert(v.isObject());
    assert(v.isMember("envelope") && v["envelope"].isObject());
    assert(v.isMember("canon_json_hex") && v["canon_json_hex"].isString());
    assert(v.isMember("canon_cbor_hex") && v["canon_cbor_hex"].isString());

    Json::Value env = v["envelope"];
    assert(env.isMember("auth") && env["auth"].isObject());

    // JSON canonical bytes: based on auth.alg="ed25519" (vector convention).
    {
      env["auth"]["alg"] = "ed25519";
      const Json::Value no_sig = env_no_sig_for(env);
      std::string got;
      std::string gerr;
      assert(canonical_json_bytes(no_sig, &got, &gerr));
      const std::string want = bytes_from_hex(v["canon_json_hex"].asString());
      assert(got == want);
    }

    // CBOR canonical bytes: based on auth.alg="ed25519-cbor" (vector convention).
    {
      env["auth"]["alg"] = "ed25519-cbor";
      const Json::Value no_sig = env_no_sig_for(env);
      std::string got;
      std::string gerr;
      assert(agentd::cbor_encode_json_value(no_sig, &got, &gerr));
      const std::string want = bytes_from_hex(v["canon_cbor_hex"].asString());
      assert(got == want);
    }

    // HMAC signatures (if present).
    if (v.isMember("hmac") && v["hmac"].isObject()) {
      const Json::Value h = v["hmac"];
      assert(h.isMember("secret_ascii") && h["secret_ascii"].isString());
      const std::string sec = h["secret_ascii"].asString();

      if (h.isMember("sig_b64_json_alg_hmac_sha256") && h["sig_b64_json_alg_hmac_sha256"].isString()) {
        env["auth"]["alg"] = "hmac-sha256";
        const Json::Value no_sig = env_no_sig_for(env);
        std::string input;
        std::string ierr;
        assert(canonical_json_bytes(no_sig, &input, &ierr));
        uint8_t mac[32];
        agent_hmac_sha256(sec.data(), sec.size(), input.data(), input.size(), mac);
        const std::string got = base64_encode(mac, sizeof(mac));
        assert(got == h["sig_b64_json_alg_hmac_sha256"].asString());
      }

      if (h.isMember("sig_b64_cbor_alg_hmac_sha256_cbor") && h["sig_b64_cbor_alg_hmac_sha256_cbor"].isString()) {
        env["auth"]["alg"] = "hmac-sha256-cbor";
        const Json::Value no_sig = env_no_sig_for(env);
        std::string input;
        std::string ierr;
        assert(agentd::cbor_encode_json_value(no_sig, &input, &ierr));
        uint8_t mac[32];
        agent_hmac_sha256(sec.data(), sec.size(), input.data(), input.size(), mac);
        const std::string got = base64_encode(mac, sizeof(mac));
        assert(got == h["sig_b64_cbor_alg_hmac_sha256_cbor"].asString());
      }
    }

    // Ed25519 signatures (if present).
    if (v.isMember("ed25519") && v["ed25519"].isObject()) {
      const Json::Value e = v["ed25519"];
      assert(e.isMember("sk_seed_hex") && e["sk_seed_hex"].isString());
      const std::string sk_hex = e["sk_seed_hex"].asString();
      assert(sk_hex.size() == 64);

      uint8_t sk[32];
      for (size_t j = 0; j < 32; j++) {
        const int hi = hex_val(sk_hex[2 * j]);
        const int lo = hex_val(sk_hex[2 * j + 1]);
        assert(hi >= 0 && lo >= 0);
        sk[j] = (uint8_t)((hi << 4) | lo);
      }
      uint8_t pk[32];
      agent_ed25519_publickey(sk, pk);

      if (e.isMember("sig_b64_json_alg_ed25519") && e["sig_b64_json_alg_ed25519"].isString()) {
        env["auth"]["alg"] = "ed25519";
        const Json::Value no_sig = env_no_sig_for(env);
        std::string input;
        std::string ierr;
        assert(canonical_json_bytes(no_sig, &input, &ierr));
        uint8_t sig[64];
        agent_ed25519_sign(input.data(), input.size(), sk, pk, sig);
        assert(base64_encode(sig, sizeof(sig)) == e["sig_b64_json_alg_ed25519"].asString());
      }
      if (e.isMember("sig_b64_cbor_alg_ed25519_cbor") && e["sig_b64_cbor_alg_ed25519_cbor"].isString()) {
        env["auth"]["alg"] = "ed25519-cbor";
        const Json::Value no_sig = env_no_sig_for(env);
        std::string input;
        std::string ierr;
        assert(agentd::cbor_encode_json_value(no_sig, &input, &ierr));
        uint8_t sig[64];
        agent_ed25519_sign(input.data(), input.size(), sk, pk, sig);
        assert(base64_encode(sig, sizeof(sig)) == e["sig_b64_cbor_alg_ed25519_cbor"].asString());
      }
    }
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: test_umbmp_auth_vectors <path-to-vectors.json>\n");
    return 2;
  }
  test_vectors_file(argv[1] ? argv[1] : "");
  std::printf("umbmp_auth_vectors_tests OK\n");
  return 0;
}
