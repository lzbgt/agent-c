#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "agent/cbor_det.h"
#include "agent/edge_interop.h"
#include "agent/umbmp_auth.h"
#include "agent/um_eais_node_caps_rsp_write.h"
#include "agent/um_eais_node_write.h"
}

static void usage() {
  std::fprintf(stderr,
               "agent_core_umbmp_cbor_encode\n\n"
               "Emits a deterministic CBOR UM-BMP envelope to stdout (no HTTP).\n\n"
               "Required:\n"
               "  --type <NODE_HELLO|NODE_CAPS_RSP>\n"
               "  --node-id <id>\n"
               "  --msg-id <id>\n\n"
               "Optional:\n"
               "  --ts-utc-ms <ms>          default: now\n"
               "  --model <s>              NODE_HELLO only (default: esp32)\n"
               "  --fw-git-sha <s>          NODE_HELLO only (default: deadbeef)\n"
               "  --caps-sha256 <token>     NODE_HELLO and minimal manifest\n"
               "  --manifest-minimal        NODE_CAPS_RSP: generate a small deterministic manifest\n"
               "  --manifest-cbor-hex <hex> NODE_CAPS_RSP: use caller-provided manifest CBOR bytes\n"
               "  --enforce-det             NODE_CAPS_RSP: enforce deterministic manifest key ordering\n\n"
               "Envelope auth (optional; CBOR signing):\n"
               "  --auth-alg hmac-sha256-cbor\n"
               "  --auth-kid <kid>\n"
               "  --auth-seq <u64>\n"
               "  --hmac-secret <ascii>\n");
}

static bool starts_with(const std::string& s, const char* pfx) {
  const size_t n = std::strlen(pfx);
  return s.size() >= n && std::memcmp(s.data(), pfx, n) == 0;
}

static uint8_t hex_nibble(char c, bool* ok) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
  if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
  *ok = false;
  return 0;
}

static bool parse_hex_bytes(const std::string& hex, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  std::string h = hex;
  if (starts_with(h, "0x")) h = h.substr(2);
  if ((h.size() % 2) != 0) return false;
  out->reserve(h.size() / 2);
  for (size_t i = 0; i < h.size(); i += 2) {
    bool ok = true;
    const uint8_t hi = hex_nibble(h[i], &ok);
    const uint8_t lo = hex_nibble(h[i + 1], &ok);
    if (!ok) return false;
    out->push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

static int64_t now_utc_ms() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  return (int64_t)ms;
}

static bool parse_i64(const std::string& s, int64_t* out) {
  if (!out) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  *out = (int64_t)v;
  return true;
}

static bool parse_u64(const std::string& s, uint64_t* out) {
  if (!out) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  *out = (uint64_t)v;
  return true;
}

static agent_status_t encode_null_trace(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_null(w);
}

static agent_status_t encode_text(agent_cbor_writer_t* w, void* ctx) {
  const agent_cbor_text_view_t* tv = (const agent_cbor_text_view_t*)ctx;
  if (!tv || !tv->ptr) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, tv->ptr, tv->len);
}

typedef struct manifest_minimal_ctx {
  agent_cbor_text_view_t node_id;
  agent_cbor_text_view_t caps_sha256;
  int has_caps_sha256;
} manifest_minimal_ctx_t;

static agent_status_t encode_empty_array(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_array_start(w, 0);
}

static agent_status_t encode_node_obj(agent_cbor_writer_t* w, void* ctx) {
  const manifest_minimal_ctx_t* m = (const manifest_minimal_ctx_t*)ctx;
  if (!m) return AGENT_ERR_INVALID_ARGUMENT;

  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "node_id",
      .key_len = 7,
      .encode_value = encode_text,
      .value_ctx = (void*)&m->node_id,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_manifest_minimal(agent_cbor_writer_t* w, void* ctx) {
  const manifest_minimal_ctx_t* m = (const manifest_minimal_ctx_t*)ctx;
  if (!m) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_text_view_t spec = {.ptr = "um-acds/0.1", .len = std::strlen("um-acds/0.1")};
  agent_cbor_text_view_t ver = {.ptr = "0.0.1", .len = std::strlen("0.0.1")};

  agent_cbor_kv_t kv[5];
  size_t n = 0;

  kv[n++] = (agent_cbor_kv_t){
    .key = "node",
    .key_len = 4,
    .encode_value = encode_node_obj,
    .value_ctx = (void*)m,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "tools",
    .key_len = 5,
    .encode_value = encode_empty_array,
    .value_ctx = NULL,
  };
  if (m->has_caps_sha256) {
    kv[n++] = (agent_cbor_kv_t){
      .key = "caps_sha256",
      .key_len = 11,
      .encode_value = encode_text,
      .value_ctx = (void*)&m->caps_sha256,
    };
  }
  kv[n++] = (agent_cbor_kv_t){
    .key = "spec_version",
    .key_len = 12,
    .encode_value = encode_text,
    .value_ctx = (void*)&spec,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "manifest_version",
    .key_len = 16,
    .encode_value = encode_text,
    .value_ctx = (void*)&ver,
  };

  return agent_cbor_write_map_sorted(w, kv, n);
}

int main(int argc, char** argv) {
  std::string type;
  std::string node_id;
  std::string msg_id;
  std::string model = "esp32";
  std::string fw_git_sha = "deadbeef";
  std::string caps_sha256;
  int64_t ts_utc_ms = 0;
  bool has_ts = false;

  bool manifest_minimal = false;
  std::string manifest_hex;
  bool enforce_det = false;

  std::string auth_alg;
  std::string auth_kid;
  uint64_t auth_seq = 0;
  bool has_auth_seq = false;
  std::string hmac_secret;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto need_value = [&](std::string* out) -> bool {
      if (!out) return false;
      if (i + 1 >= argc) return false;
      *out = argv[++i] ? argv[i] : "";
      return true;
    };

    if (a == "--type") {
      if (!need_value(&type)) return (usage(), 2);
    } else if (a == "--node-id") {
      if (!need_value(&node_id)) return (usage(), 2);
    } else if (a == "--msg-id") {
      if (!need_value(&msg_id)) return (usage(), 2);
    } else if (a == "--ts-utc-ms") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_i64(v, &ts_utc_ms)) {
        std::fprintf(stderr, "invalid --ts-utc-ms\n");
        return 2;
      }
      has_ts = true;
    } else if (a == "--model") {
      if (!need_value(&model)) return (usage(), 2);
    } else if (a == "--fw-git-sha") {
      if (!need_value(&fw_git_sha)) return (usage(), 2);
    } else if (a == "--caps-sha256") {
      if (!need_value(&caps_sha256)) return (usage(), 2);
    } else if (a == "--manifest-minimal") {
      manifest_minimal = true;
    } else if (a == "--manifest-cbor-hex") {
      if (!need_value(&manifest_hex)) return (usage(), 2);
    } else if (a == "--enforce-det") {
      enforce_det = true;
    } else if (a == "--auth-alg") {
      if (!need_value(&auth_alg)) return (usage(), 2);
    } else if (a == "--auth-kid") {
      if (!need_value(&auth_kid)) return (usage(), 2);
    } else if (a == "--auth-seq") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_u64(v, &auth_seq)) {
        std::fprintf(stderr, "invalid --auth-seq\n");
        return 2;
      }
      has_auth_seq = true;
    } else if (a == "--hmac-secret") {
      if (!need_value(&hmac_secret)) return (usage(), 2);
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  if (type.empty() || node_id.empty() || msg_id.empty()) {
    usage();
    return 2;
  }
  if (!has_ts) ts_utc_ms = now_utc_ms();

  if (!agent_umbmp_id_is_safe(node_id.c_str(), node_id.size())) {
    std::fprintf(stderr, "invalid --node-id (id-safe required)\n");
    return 2;
  }
  if (!agent_umbmp_id_is_safe(msg_id.c_str(), msg_id.size())) {
    std::fprintf(stderr, "invalid --msg-id (id-safe required)\n");
    return 2;
  }
  if (!caps_sha256.empty() && !agent_umbmp_sha256_token_is_safe(caps_sha256.c_str(), caps_sha256.size())) {
    std::fprintf(stderr, "invalid --caps-sha256 token\n");
    return 2;
  }

  std::string from = "node:" + node_id;
  const std::string to = "platform";

  const bool want_auth = !auth_alg.empty() || !auth_kid.empty() || has_auth_seq || !hmac_secret.empty();
  if (want_auth) {
    if (auth_alg != "hmac-sha256-cbor") {
      std::fprintf(stderr, "unsupported --auth-alg (supported: hmac-sha256-cbor)\n");
      return 2;
    }
    if (auth_kid.empty() || !agent_umbmp_id_is_safe(auth_kid.c_str(), auth_kid.size())) {
      std::fprintf(stderr, "invalid/missing --auth-kid (id-safe required)\n");
      return 2;
    }
    if (!has_auth_seq) {
      std::fprintf(stderr, "missing --auth-seq\n");
      return 2;
    }
    if (hmac_secret.empty()) {
      std::fprintf(stderr, "missing --hmac-secret\n");
      return 2;
    }
  }

  // Encode envelope into a bounded buffer and emit raw bytes to stdout.
  uint8_t buf[64 * 1024];
  agent_cbor_writer_t w{};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  agent_umbmp_envelope_cbor_params_t p{};
  p.msg_id = msg_id.c_str();
  p.msg_id_len = msg_id.size();
  p.ts_utc_ms = ts_utc_ms;
  p.type = type.c_str();
  p.type_len = type.size();
  p.from = from.c_str();
  p.from_len = from.size();
  p.to = to.c_str();
  p.to_len = to.size();
  p.encode_trace = encode_null_trace;
  p.trace_ctx = NULL;

  agent_status_t st = AGENT_ERR_INVALID_ARGUMENT;

  // When auth is requested, compute auth.sig over the deterministic env_no_sig CBOR bytes
  // and include it in the final envelope bytes.
  char sig_b64[256];
  size_t sig_b64_len = 0;
  uint8_t signing_buf[64 * 1024];
  agent_cbor_writer_t sw{};
  if (want_auth) {
    p.auth_alg = auth_alg.c_str();
    p.auth_alg_len = auth_alg.size();
    p.auth_kid = auth_kid.c_str();
    p.auth_kid_len = auth_kid.size();
    p.auth_seq = auth_seq;
    p.auth_has_seq = 1;
    agent_cbor_writer_init(&sw, signing_buf, sizeof(signing_buf));
  }

  if (type == "NODE_HELLO") {
    agent_um_eais_node_hello_body_t b{};
    b.node_id = {node_id.c_str(), node_id.size()};
    if (!model.empty()) {
      b.model = {model.c_str(), model.size()};
      b.has_model = 1;
    }
    if (!fw_git_sha.empty()) {
      b.fw_git_sha = {fw_git_sha.c_str(), fw_git_sha.size()};
      b.has_fw_git_sha = 1;
    }
    if (!caps_sha256.empty()) {
      b.caps_sha256 = {caps_sha256.c_str(), caps_sha256.size()};
      b.has_caps_sha256 = 1;
    }
    p.encode_body = agent_um_eais_node_hello_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    if (want_auth) {
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &sw);
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to build signing input\n");
        return 1;
      }
      st = agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
        hmac_secret.data(),
        hmac_secret.size(),
        agent_cbor_writer_bytes(&sw),
        agent_cbor_writer_len(&sw),
        sig_b64,
        sizeof(sig_b64),
        &sig_b64_len
      );
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to compute hmac sig\n");
        return 1;
      }
      p.auth_sig_b64 = sig_b64;
      p.auth_sig_b64_len = sig_b64_len;
      st = agent_umbmp_envelope_cbor_v0_4(&p, &w);
    } else {
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w);
    }
  } else if (type == "NODE_CAPS_RSP") {
    std::vector<uint8_t> manifest_bytes;
    agent_cbor_view_t man{};

    uint8_t man_buf[16 * 1024];
    agent_cbor_writer_t mw{};
    agent_cbor_writer_init(&mw, man_buf, sizeof(man_buf));

    if (!manifest_hex.empty()) {
      if (!parse_hex_bytes(manifest_hex, &manifest_bytes) || manifest_bytes.empty()) {
        std::fprintf(stderr, "invalid --manifest-cbor-hex\n");
        return 2;
      }
      man = {manifest_bytes.data(), manifest_bytes.size()};
    } else if (manifest_minimal) {
      manifest_minimal_ctx_t mctx{};
      mctx.node_id = {node_id.c_str(), node_id.size()};
      if (!caps_sha256.empty()) {
        mctx.caps_sha256 = {caps_sha256.c_str(), caps_sha256.size()};
        mctx.has_caps_sha256 = 1;
      }
      if (encode_manifest_minimal(&mw, &mctx) != AGENT_OK) {
        std::fprintf(stderr, "failed to encode minimal manifest\n");
        return 1;
      }
      man = {agent_cbor_writer_bytes(&mw), agent_cbor_writer_len(&mw)};
    } else {
      std::fprintf(stderr, "NODE_CAPS_RSP requires --manifest-minimal or --manifest-cbor-hex\n");
      return 2;
    }

    agent_um_eais_node_caps_rsp_body_t b{};
    b.node_id = {node_id.c_str(), node_id.size()};
    b.manifest_cbor = man;
    b.enforce_deterministic_keys = enforce_det ? 1 : 0;

    p.encode_body = agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    if (want_auth) {
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &sw);
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to build signing input\n");
        return 1;
      }
      st = agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
        hmac_secret.data(),
        hmac_secret.size(),
        agent_cbor_writer_bytes(&sw),
        agent_cbor_writer_len(&sw),
        sig_b64,
        sizeof(sig_b64),
        &sig_b64_len
      );
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to compute hmac sig\n");
        return 1;
      }
      p.auth_sig_b64 = sig_b64;
      p.auth_sig_b64_len = sig_b64_len;
      st = agent_umbmp_envelope_cbor_v0_4(&p, &w);
    } else {
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w);
    }
  } else {
    std::fprintf(stderr, "unsupported --type: %s\n", type.c_str());
    usage();
    return 2;
  }

  if (st != AGENT_OK) {
    std::fprintf(stderr, "encode failed status=%d\n", (int)st);
    return 1;
  }

  const uint8_t* out = agent_cbor_writer_bytes(&w);
  const size_t out_len = agent_cbor_writer_len(&w);
  if (!out || out_len == 0) {
    std::fprintf(stderr, "encode produced empty output\n");
    return 1;
  }

  if (std::fwrite(out, 1, out_len, stdout) != out_len) {
    std::fprintf(stderr, "stdout write failed\n");
    return 1;
  }
  return 0;
}
