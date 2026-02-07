#include "agent/umbmp_auth.h"
#include "agent/um_eais_node_write.h"

#include "cbor_decode.h"
#include "cbor_encode.h"

#include <json/json.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <string>

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

static void assert_cbor_roundtrip_stable(
  const std::string& name,
  const std::string& core_cbor_bytes
) {
  std::string derr;
  Json::Value env;
  assert(agentd::cbor_decode_to_json_value(core_cbor_bytes, &env, &derr) && derr.empty());
  assert(env.isObject());

  std::string daemon_cbor_bytes;
  std::string eerr;
  assert(agentd::cbor_encode_json_value(env, &daemon_cbor_bytes, &eerr) && eerr.empty());

  if (daemon_cbor_bytes != core_cbor_bytes) {
    std::fprintf(stderr, "CBOR roundtrip mismatch: %s\n", name.c_str());
    std::fprintf(stderr, "core  hex=%s\n", hex_of_bytes(core_cbor_bytes).c_str());
    std::fprintf(stderr, "daemon hex=%s\n", hex_of_bytes(daemon_cbor_bytes).c_str());
    std::abort();
  }
}

static std::string build_core_env_no_sig_node_heartbeat_with_integral_floats(void) {
  // This reproduces a previously observed bug:
  // - node encodes telemetry as float64 (e.g. 87.0, -55.0)
  // - platform decodes CBOR -> JSON and re-encodes to CBOR
  // - if the platform "helpfully" encoded integral-looking doubles as ints,
  //   envelope auth (hmac-sha256-cbor / ed25519-cbor) would fail.
  //
  // Here we assert the daemon CBOR encoder preserves the numeric type.
  uint8_t buf[16 * 1024];
  agent_cbor_writer_t w{};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  agent_um_eais_node_heartbeat_body_t body{};
  const std::string node_id = "node_roundtrip_1";
  body.node_id = {node_id.c_str(), node_id.size()};

  const int health_ok = 1;
  body.encode_health =
    [](agent_cbor_writer_t* w2, void* ctx) -> agent_status_t {
      const int* ok = (const int*)ctx;
      const agent_cbor_kv_t kv[] = {
        (agent_cbor_kv_t){
          .key = "ok",
          .key_len = 2,
          .encode_value =
            [](agent_cbor_writer_t* w3, void* ctx2) -> agent_status_t {
              const int* ok2 = (const int*)ctx2;
              return agent_cbor_write_bool(w3, ok2 && *ok2 ? 1 : 0);
            },
          .value_ctx = (void*)ok,
        },
      };
      return agent_cbor_write_map_sorted(w2, kv, 1);
    };
  body.health_ctx = (void*)&health_ok;

  // Integers represented as float64 on purpose.
  body.has_battery_pct = 1;
  body.battery_pct = 87.0;
  body.has_rssi = 1;
  body.rssi = -55.0;

  // Envelope without signature (the exact signing input for *-cbor algs).
  agent_umbmp_envelope_cbor_params_t p{};
  const std::string msg_id = "00000000-0000-4000-8000-0000000000ff";
  const std::string type = "NODE_HEARTBEAT";
  const std::string from = "node:" + node_id;
  const std::string to = "platform";
  const std::string auth_alg = "hmac-sha256-cbor";
  const std::string auth_kid = node_id;

  p.msg_id = msg_id.c_str();
  p.msg_id_len = msg_id.size();
  p.ts_utc_ms = 1700000000123LL;
  p.type = type.c_str();
  p.type_len = type.size();
  p.from = from.c_str();
  p.from_len = from.size();
  p.to = to.c_str();
  p.to_len = to.size();
  p.encode_body = agent_um_eais_node_heartbeat_body_encode_cbor_v0_1;
  p.body_ctx = &body;
  p.encode_trace =
    [](agent_cbor_writer_t* w2, void* /*ctx*/) -> agent_status_t {
      return agent_cbor_write_null(w2);
    };
  p.trace_ctx = NULL;
  p.auth_alg = auth_alg.c_str();
  p.auth_alg_len = auth_alg.size();
  p.auth_kid = auth_kid.c_str();
  p.auth_kid_len = auth_kid.size();
  p.auth_seq = 2;
  p.auth_has_seq = 1;

  const agent_status_t st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w);
  assert(st == AGENT_OK);
  return std::string((const char*)agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));
}

int main(void) {
  {
    const std::string core_bytes = build_core_env_no_sig_node_heartbeat_with_integral_floats();
    assert_cbor_roundtrip_stable("node_heartbeat_integral_floats", core_bytes);
  }
  std::printf("test_cbor_det_roundtrip OK\n");
  return 0;
}

