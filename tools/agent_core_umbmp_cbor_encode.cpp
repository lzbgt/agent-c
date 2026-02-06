#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "agent/cbor_det.h"
#include "agent/ed25519.h"
#include "agent/edge_interop.h"
#include "agent/umbmp_auth.h"
#include "agent/um_eais_task_lifecycle_write.h"
#include "agent/um_eais_node_caps_rsp_write.h"
#include "agent/um_eais_node_write.h"
}

static void usage() {
  std::fprintf(stderr,
               "agent_core_umbmp_cbor_encode\n\n"
               "Emits a deterministic CBOR UM-BMP envelope to stdout (no HTTP).\n\n"
               "Required:\n"
               "  --type <NODE_HELLO|NODE_CAPS_RSP|SENSOR_EVENT|TASK_ACK|TASK_EVENT|TASK_DONE|TASK_FAILED>\n"
               "  --node-id <id>\n"
               "  --msg-id <id>\n\n"
               "Optional:\n"
               "  --ts-utc-ms <ms>          default: now\n"
               "  --model <s>              NODE_HELLO only (default: esp32)\n"
               "  --fw-git-sha <s>          NODE_HELLO only (default: deadbeef)\n"
               "  --caps-sha256 <token>     NODE_HELLO and minimal manifest\n"
               "  --event-type <text>       SENSOR_EVENT only (required)\n"
               "  --confidence <float>      SENSOR_EVENT only (optional)\n"
               "  --sensor-ts-utc-ms <ms>   SENSOR_EVENT only (default: envelope ts_utc_ms)\n"
               "  --data-text <text>        SENSOR_EVENT only (optional; data:{text:<...>}; default: {})\n"
               "  --task-id <id>            TASK_* only\n"
               "  --step-id <id>            TASK_* only\n"
               "  --idempotency-key <id>    TASK_* only\n"
               "  --accepted <0|1>          TASK_ACK only\n"
               "  --reason <text>           TASK_ACK only (optional)\n"
               "  --state <text>            TASK_EVENT only (required)\n"
               "  --progress <float>        TASK_EVENT only (optional)\n"
               "  --error <text>            TASK_EVENT/TASK_FAILED only\n"
               "  --result-ok <0|1>         TASK_DONE only (default: 1)\n"
               "  --result-text <text>      TASK_DONE only (optional; result.data.text)\n"
               "  --manifest-minimal        NODE_CAPS_RSP: generate a small deterministic manifest\n"
               "  --manifest-minimal-ws2812 NODE_CAPS_RSP: deterministic manifest with tool ui.led.ws2812.control\n"
               "  --manifest-cbor-hex <hex> NODE_CAPS_RSP: use caller-provided manifest CBOR bytes\n"
               "  --enforce-det             NODE_CAPS_RSP: enforce deterministic manifest key ordering\n\n"
               "Envelope auth (optional; CBOR signing):\n"
               "  --auth-alg hmac-sha256-cbor|ed25519-cbor\n"
               "  --auth-kid <kid>\n"
               "  --auth-seq <u64>\n"
               "  --hmac-secret <ascii>     required for hmac-sha256-cbor\n"
               "  --ed25519-sk-hex <64hex>  required for ed25519-cbor (seed)\n");
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

static bool parse_double(const std::string& s, double* out) {
  if (!out) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || *end != '\0') return false;
  *out = v;
  return true;
}

static bool parse_bool01(const std::string& s, int* out) {
  if (!out) return false;
  if (s == "1" || s == "true" || s == "TRUE") {
    *out = 1;
    return true;
  }
  if (s == "0" || s == "false" || s == "FALSE") {
    *out = 0;
    return true;
  }
  return false;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static bool hex_decode_32(const std::string& hex, uint8_t out32[32]) {
  if (!out32) return false;
  if (hex.size() != 64) return false;
  for (size_t i = 0; i < 32; i++) {
    const int hi = hex_val(hex[2 * i]);
    const int lo = hex_val(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out32[i] = (uint8_t)((hi << 4) | lo);
  }
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

static agent_status_t encode_empty_map(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_map_start(w, 0);
}

static agent_status_t encode_map_text(agent_cbor_writer_t* w, void* ctx) {
  const agent_cbor_text_view_t* tv = (const agent_cbor_text_view_t*)ctx;
  if (!tv || !tv->ptr) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "text",
      .key_len = 4,
      .encode_value = encode_text,
      .value_ctx = (void*)tv,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

typedef struct encode_text_const_ctx {
  const char* s;
  size_t n;
} encode_text_const_ctx_t;

static agent_status_t encode_text_const(agent_cbor_writer_t* w, void* ctx) {
  const encode_text_const_ctx_t* c = (const encode_text_const_ctx_t*)ctx;
  if (!c || !c->s) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, c->s, c->n);
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

static agent_status_t encode_schema_action(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t type = {.s = "string", .n = std::strlen("string")};
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "type",
      .key_len = 4,
      .encode_value = encode_text_const,
      .value_ctx = &type,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_schema_text(agent_cbor_writer_t* w, void* ctx) {
  return encode_schema_action(w, ctx);
}

static agent_status_t encode_schema_properties_action(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "action",
      .key_len = 6,
      .encode_value = encode_schema_action,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_schema_properties_text(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "text",
      .key_len = 4,
      .encode_value = encode_schema_text,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_schema_required_action(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  agent_status_t st = agent_cbor_write_array_start(w, 1);
  if (st != AGENT_OK) return st;
  return agent_cbor_write_text(w, "action", std::strlen("action"));
}

static agent_status_t encode_schema_required_text(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  agent_status_t st = agent_cbor_write_array_start(w, 1);
  if (st != AGENT_OK) return st;
  return agent_cbor_write_text(w, "text", std::strlen("text"));
}

static agent_status_t encode_parameters_schema_ws2812(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t type = {.s = "object", .n = std::strlen("object")};
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "type",
      .key_len = 4,
      .encode_value = encode_text_const,
      .value_ctx = &type,
    },
    (agent_cbor_kv_t){
      .key = "additionalProperties",
      .key_len = 20,
      .encode_value =
        [](agent_cbor_writer_t* w2, void* ctx2) -> agent_status_t {
        (void)ctx2;
        return agent_cbor_write_bool(w2, 0);
      },
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "properties",
      .key_len = 10,
      .encode_value = encode_schema_properties_action,
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "required",
      .key_len = 8,
      .encode_value = encode_schema_required_action,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 4);
}

static agent_status_t encode_result_schema_text(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t type = {.s = "object", .n = std::strlen("object")};
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "type",
      .key_len = 4,
      .encode_value = encode_text_const,
      .value_ctx = &type,
    },
    (agent_cbor_kv_t){
      .key = "additionalProperties",
      .key_len = 20,
      .encode_value =
        [](agent_cbor_writer_t* w2, void* ctx2) -> agent_status_t {
        (void)ctx2;
        return agent_cbor_write_bool(w2, 0);
      },
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "properties",
      .key_len = 10,
      .encode_value = encode_schema_properties_text,
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "required",
      .key_len = 8,
      .encode_value = encode_schema_required_text,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 4);
}

static agent_status_t encode_one_tool_ws2812(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t name = {.s = "ui.led.ws2812.control", .n = std::strlen("ui.led.ws2812.control")};
  encode_text_const_ctx_t kind = {.s = "actuator", .n = std::strlen("actuator")};
  encode_text_const_ctx_t desc = {.s = "Control LED", .n = std::strlen("Control LED")};
  encode_text_const_ctx_t side = {.s = "low", .n = std::strlen("low")};

  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "name",
      .key_len = 4,
      .encode_value = encode_text_const,
      .value_ctx = &name,
    },
    (agent_cbor_kv_t){
      .key = "kind",
      .key_len = 4,
      .encode_value = encode_text_const,
      .value_ctx = &kind,
    },
    (agent_cbor_kv_t){
      .key = "description",
      .key_len = 11,
      .encode_value = encode_text_const,
      .value_ctx = &desc,
    },
    (agent_cbor_kv_t){
      .key = "parameters_schema",
      .key_len = 17,
      .encode_value = encode_parameters_schema_ws2812,
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "result_schema",
      .key_len = 13,
      .encode_value = encode_result_schema_text,
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "timeout_ms",
      .key_len = 10,
      .encode_value =
        [](agent_cbor_writer_t* w2, void* ctx2) -> agent_status_t {
        (void)ctx2;
        return agent_cbor_write_uint(w2, 500);
      },
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "idempotent",
      .key_len = 10,
      .encode_value =
        [](agent_cbor_writer_t* w2, void* ctx2) -> agent_status_t {
        (void)ctx2;
        return agent_cbor_write_bool(w2, 0);
      },
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "side_effect_level",
      .key_len = 17,
      .encode_value = encode_text_const,
      .value_ctx = &side,
    },
    (agent_cbor_kv_t){
      .key = "hazards",
      .key_len = 7,
      .encode_value = encode_empty_array,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 9);
}

static agent_status_t encode_tools_array_ws2812(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  agent_status_t st = agent_cbor_write_array_start(w, 1);
  if (st != AGENT_OK) return st;
  return encode_one_tool_ws2812(w, NULL);
}

static agent_status_t encode_hardware_presence(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t val = {.s = "present", .n = std::strlen("present")};
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "ui.led.ws2812",
      .key_len = 13,
      .encode_value = encode_text_const,
      .value_ctx = &val,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_hardware_obj(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "presence",
      .key_len = 8,
      .encode_value = encode_hardware_presence,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_agent_core_runtime(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  encode_text_const_ctx_t ver = {.s = "0.0.0", .n = std::strlen("0.0.0")};
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "version",
      .key_len = 7,
      .encode_value = encode_text_const,
      .value_ctx = &ver,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_runtime_obj(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "agent_core",
      .key_len = 10,
      .encode_value = encode_agent_core_runtime,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_tags_room_lobby(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  agent_status_t st = agent_cbor_write_array_start(w, 1);
  if (st != AGENT_OK) return st;
  return agent_cbor_write_text(w, "room:lobby", std::strlen("room:lobby"));
}

static agent_status_t encode_manifest_minimal_ws2812(agent_cbor_writer_t* w, void* ctx) {
  const manifest_minimal_ctx_t* m = (const manifest_minimal_ctx_t*)ctx;
  if (!m) return AGENT_ERR_INVALID_ARGUMENT;

  encode_text_const_ctx_t spec = {.s = "um-acds/0.1", .n = std::strlen("um-acds/0.1")};
  encode_text_const_ctx_t ver = {.s = "0.0.1", .n = std::strlen("0.0.1")};

  agent_cbor_kv_t kv[9];
  size_t n = 0;

  kv[n++] = (agent_cbor_kv_t){
    .key = "spec_version",
    .key_len = 12,
    .encode_value = encode_text_const,
    .value_ctx = &spec,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "manifest_version",
    .key_len = 16,
    .encode_value = encode_text_const,
    .value_ctx = &ver,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "node",
    .key_len = 4,
    .encode_value = encode_node_obj,
    .value_ctx = (void*)m,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "runtime",
    .key_len = 7,
    .encode_value = encode_runtime_obj,
    .value_ctx = NULL,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "hardware",
    .key_len = 8,
    .encode_value = encode_hardware_obj,
    .value_ctx = NULL,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "tools",
    .key_len = 5,
    .encode_value = encode_tools_array_ws2812,
    .value_ctx = NULL,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "safety",
    .key_len = 6,
    .encode_value = encode_empty_map,
    .value_ctx = NULL,
  };
  kv[n++] = (agent_cbor_kv_t){
    .key = "tags",
    .key_len = 4,
    .encode_value = encode_tags_room_lobby,
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

  return agent_cbor_write_map_sorted(w, kv, n);
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

typedef struct encode_task_done_result_ctx {
  int ok;  // boolean
  agent_cbor_text_view_t text;  // optional => result.data.text
  int has_text;
} encode_task_done_result_ctx_t;

static agent_status_t encode_task_done_result_data(agent_cbor_writer_t* w, void* ctx) {
  const encode_task_done_result_ctx_t* r = (const encode_task_done_result_ctx_t*)ctx;
  if (!r || !r->has_text) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "text",
      .key_len = 4,
      .encode_value = encode_text,
      .value_ctx = (void*)&r->text,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_task_done_result(agent_cbor_writer_t* w, void* ctx) {
  const encode_task_done_result_ctx_t* r = (const encode_task_done_result_ctx_t*)ctx;
  if (!r) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t kv[2];
  size_t n = 0;
  kv[n++] = (agent_cbor_kv_t){
    .key = "ok",
    .key_len = 2,
    .encode_value =
      [](agent_cbor_writer_t* w2, void* ctx2) -> agent_status_t {
      const encode_task_done_result_ctx_t* rr = (const encode_task_done_result_ctx_t*)ctx2;
      if (!rr) return AGENT_ERR_INVALID_ARGUMENT;
      return agent_cbor_write_bool(w2, rr->ok ? 1 : 0);
    },
    .value_ctx = (void*)r,
  };
  if (r->has_text) {
    kv[n++] = (agent_cbor_kv_t){
      .key = "data",
      .key_len = 4,
      .encode_value = encode_task_done_result_data,
      .value_ctx = (void*)r,
    };
  }
  return agent_cbor_write_map_sorted(w, kv, n);
}

static agent_status_t compute_auth_sig_b64(
  const std::string& auth_alg,
  const std::string& hmac_secret,
  const std::string& ed25519_sk_hex,
  const uint8_t* signing_input,
  size_t signing_input_len,
  char* out_b64,
  size_t out_cap,
  size_t* out_len
) {
  if (!out_b64 || !out_len) return AGENT_ERR_INVALID_ARGUMENT;
  *out_len = 0;
  if (auth_alg == "hmac-sha256-cbor") {
    return agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
      hmac_secret.data(),
      hmac_secret.size(),
      signing_input,
      signing_input_len,
      out_b64,
      out_cap,
      out_len
    );
  }
  if (auth_alg == "ed25519-cbor") {
    uint8_t sk_seed32[32];
    if (!hex_decode_32(ed25519_sk_hex, sk_seed32)) return AGENT_ERR_INVALID_ARGUMENT;
    uint8_t pk32[32];
    std::memset(pk32, 0, sizeof(pk32));
    agent_ed25519_publickey(sk_seed32, pk32);
    return agent_umbmp_auth_ed25519_cbor_sig_b64(
      sk_seed32,
      pk32,
      signing_input,
      signing_input_len,
      out_b64,
      out_cap,
      out_len
    );
  }
  return AGENT_ERR_INVALID_ARGUMENT;
}

int main(int argc, char** argv) {
  std::string type;
  std::string node_id;
  std::string msg_id;
  std::string model = "esp32";
  std::string fw_git_sha = "deadbeef";
  std::string caps_sha256;
  std::string task_id;
  std::string step_id;
  std::string idempotency_key;
  std::string reason;
  std::string state;
  std::string error;
  std::string result_text;
  std::string event_type;
  std::string data_text;
  int accepted = 1;
  bool has_accepted = false;
  double confidence = 0.0;
  bool has_confidence = false;
  double progress = 0.0;
  bool has_progress = false;
  int result_ok = 1;
  int64_t ts_utc_ms = 0;
  bool has_ts = false;
  int64_t sensor_ts_utc_ms = 0;
  bool has_sensor_ts = false;

  bool manifest_minimal = false;
  bool manifest_minimal_ws2812 = false;
  std::string manifest_hex;
  bool enforce_det = false;

  std::string auth_alg;
  std::string auth_kid;
  uint64_t auth_seq = 0;
  bool has_auth_seq = false;
  std::string hmac_secret;
  std::string ed25519_sk_hex;

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
    } else if (a == "--event-type") {
      if (!need_value(&event_type)) return (usage(), 2);
    } else if (a == "--confidence") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_double(v, &confidence)) {
        std::fprintf(stderr, "invalid --confidence\n");
        return 2;
      }
      has_confidence = true;
    } else if (a == "--sensor-ts-utc-ms") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_i64(v, &sensor_ts_utc_ms)) {
        std::fprintf(stderr, "invalid --sensor-ts-utc-ms\n");
        return 2;
      }
      has_sensor_ts = true;
    } else if (a == "--data-text") {
      if (!need_value(&data_text)) return (usage(), 2);
    } else if (a == "--task-id") {
      if (!need_value(&task_id)) return (usage(), 2);
    } else if (a == "--step-id") {
      if (!need_value(&step_id)) return (usage(), 2);
    } else if (a == "--idempotency-key") {
      if (!need_value(&idempotency_key)) return (usage(), 2);
    } else if (a == "--accepted") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_bool01(v, &accepted)) {
        std::fprintf(stderr, "invalid --accepted (expected 0/1)\n");
        return 2;
      }
      has_accepted = true;
    } else if (a == "--reason") {
      if (!need_value(&reason)) return (usage(), 2);
    } else if (a == "--state") {
      if (!need_value(&state)) return (usage(), 2);
    } else if (a == "--progress") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_double(v, &progress)) {
        std::fprintf(stderr, "invalid --progress\n");
        return 2;
      }
      has_progress = true;
    } else if (a == "--error") {
      if (!need_value(&error)) return (usage(), 2);
    } else if (a == "--result-ok") {
      std::string v;
      if (!need_value(&v)) return (usage(), 2);
      if (!parse_bool01(v, &result_ok)) {
        std::fprintf(stderr, "invalid --result-ok (expected 0/1)\n");
        return 2;
      }
    } else if (a == "--result-text") {
      if (!need_value(&result_text)) return (usage(), 2);
    } else if (a == "--manifest-minimal") {
      manifest_minimal = true;
    } else if (a == "--manifest-minimal-ws2812") {
      manifest_minimal_ws2812 = true;
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
    } else if (a == "--ed25519-sk-hex") {
      if (!need_value(&ed25519_sk_hex)) return (usage(), 2);
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
  if (!task_id.empty() && !agent_umbmp_id_is_safe(task_id.c_str(), task_id.size())) {
    std::fprintf(stderr, "invalid --task-id (id-safe required)\n");
    return 2;
  }
  if (!step_id.empty() && !agent_umbmp_id_is_safe(step_id.c_str(), step_id.size())) {
    std::fprintf(stderr, "invalid --step-id (id-safe required)\n");
    return 2;
  }
  if (!idempotency_key.empty() && !agent_umbmp_id_is_safe(idempotency_key.c_str(), idempotency_key.size())) {
    std::fprintf(stderr, "invalid --idempotency-key (id-safe required)\n");
    return 2;
  }
  if (!caps_sha256.empty() && !agent_umbmp_sha256_token_is_safe(caps_sha256.c_str(), caps_sha256.size())) {
    std::fprintf(stderr, "invalid --caps-sha256 token\n");
    return 2;
  }

  std::string from = "node:" + node_id;
  const std::string to = "platform";

  const bool want_auth =
    !auth_alg.empty() || !auth_kid.empty() || has_auth_seq || !hmac_secret.empty() || !ed25519_sk_hex.empty();
  if (want_auth) {
    if (auth_alg != "hmac-sha256-cbor" && auth_alg != "ed25519-cbor") {
      std::fprintf(stderr, "unsupported --auth-alg (supported: hmac-sha256-cbor|ed25519-cbor)\n");
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
    if (auth_alg == "hmac-sha256-cbor") {
      if (hmac_secret.empty()) {
        std::fprintf(stderr, "missing --hmac-secret\n");
        return 2;
      }
    } else if (auth_alg == "ed25519-cbor") {
      if (ed25519_sk_hex.empty()) {
        std::fprintf(stderr, "missing --ed25519-sk-hex\n");
        return 2;
      }
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
  }

  auto encode_now = [&]() -> int {
    if (want_auth) {
      agent_cbor_writer_init(&sw, signing_buf, sizeof(signing_buf));
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &sw);
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to build signing input\n");
        return 1;
      }
      if (auth_alg == "ed25519-cbor") {
        uint8_t tmp32[32];
        if (!hex_decode_32(ed25519_sk_hex, tmp32)) {
          std::fprintf(stderr, "invalid --ed25519-sk-hex (expected 64 hex chars)\n");
          return 2;
        }
      }
      st = compute_auth_sig_b64(
        auth_alg,
        hmac_secret,
        ed25519_sk_hex,
        agent_cbor_writer_bytes(&sw),
        agent_cbor_writer_len(&sw),
        sig_b64,
        sizeof(sig_b64),
        &sig_b64_len
      );
      if (st != AGENT_OK) {
        std::fprintf(stderr, "failed to compute auth sig\n");
        return 1;
      }
      p.auth_sig_b64 = sig_b64;
      p.auth_sig_b64_len = sig_b64_len;
      st = agent_umbmp_envelope_cbor_v0_4(&p, &w);
    } else {
      st = agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w);
    }
    return 0;
  };

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
    const int rc = encode_now();
    if (rc != 0) return rc;
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
    } else if (manifest_minimal || manifest_minimal_ws2812) {
      manifest_minimal_ctx_t mctx{};
      mctx.node_id = {node_id.c_str(), node_id.size()};
      if (!caps_sha256.empty()) {
        mctx.caps_sha256 = {caps_sha256.c_str(), caps_sha256.size()};
        mctx.has_caps_sha256 = 1;
      }
      agent_status_t mst = AGENT_ERR_INVALID_ARGUMENT;
      if (manifest_minimal_ws2812) {
        mst = encode_manifest_minimal_ws2812(&mw, &mctx);
      } else {
        mst = encode_manifest_minimal(&mw, &mctx);
      }
      if (mst != AGENT_OK) {
        std::fprintf(stderr, "failed to encode minimal manifest\n");
        return 1;
      }
      man = {agent_cbor_writer_bytes(&mw), agent_cbor_writer_len(&mw)};
    } else {
      std::fprintf(stderr, "NODE_CAPS_RSP requires --manifest-minimal, --manifest-minimal-ws2812, or --manifest-cbor-hex\n");
      return 2;
    }

    agent_um_eais_node_caps_rsp_body_t b{};
    b.node_id = {node_id.c_str(), node_id.size()};
    b.manifest_cbor = man;
    b.enforce_deterministic_keys = enforce_det ? 1 : 0;

    p.encode_body = agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
  } else if (type == "SENSOR_EVENT") {
    if (event_type.empty()) {
      std::fprintf(stderr, "SENSOR_EVENT requires --event-type\n");
      return 2;
    }
    if (!has_sensor_ts) sensor_ts_utc_ms = ts_utc_ms;

    agent_cbor_text_view_t data_tv{};
    if (!data_text.empty()) data_tv = {data_text.c_str(), data_text.size()};

    agent_um_eais_sensor_event_body_t b{};
    b.node_id = {node_id.c_str(), node_id.size()};
    b.event_type = {event_type.c_str(), event_type.size()};
    b.ts_utc_ms = sensor_ts_utc_ms;
    if (has_confidence) {
      b.confidence = confidence;
      b.has_confidence = 1;
    }
    if (!data_text.empty()) {
      b.encode_data = encode_map_text;
      b.data_ctx = &data_tv;
    } else {
      b.encode_data = encode_empty_map;
      b.data_ctx = NULL;
    }
    p.encode_body = agent_um_eais_sensor_event_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
  } else if (type == "TASK_ACK") {
    if (task_id.empty() || step_id.empty() || idempotency_key.empty()) {
      std::fprintf(stderr, "TASK_ACK requires --task-id --step-id --idempotency-key\n");
      return 2;
    }
    if (!has_accepted) {
      std::fprintf(stderr, "TASK_ACK requires --accepted\n");
      return 2;
    }
    agent_um_eais_task_ack_body_t b{};
    b.task_id = {task_id.c_str(), task_id.size()};
    b.step_id = {step_id.c_str(), step_id.size()};
    b.idempotency_key = {idempotency_key.c_str(), idempotency_key.size()};
    b.accepted = accepted ? 1 : 0;
    if (!reason.empty()) {
      b.reason = {reason.c_str(), reason.size()};
      b.has_reason = 1;
    }
    p.encode_body = agent_um_eais_task_ack_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
  } else if (type == "TASK_EVENT") {
    if (task_id.empty() || step_id.empty() || idempotency_key.empty() || state.empty()) {
      std::fprintf(stderr, "TASK_EVENT requires --task-id --step-id --idempotency-key --state\n");
      return 2;
    }
    agent_um_eais_task_event_body_t b{};
    b.task_id = {task_id.c_str(), task_id.size()};
    b.step_id = {step_id.c_str(), step_id.size()};
    b.idempotency_key = {idempotency_key.c_str(), idempotency_key.size()};
    b.state = {state.c_str(), state.size()};
    if (has_progress) {
      b.progress = progress;
      b.has_progress = 1;
    }
    if (!error.empty()) {
      b.error = {error.c_str(), error.size()};
      b.has_error = 1;
    }
    p.encode_body = agent_um_eais_task_event_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
  } else if (type == "TASK_FAILED") {
    if (task_id.empty() || step_id.empty() || idempotency_key.empty() || error.empty()) {
      std::fprintf(stderr, "TASK_FAILED requires --task-id --step-id --idempotency-key --error\n");
      return 2;
    }
    agent_um_eais_task_failed_body_t b{};
    b.task_id = {task_id.c_str(), task_id.size()};
    b.step_id = {step_id.c_str(), step_id.size()};
    b.idempotency_key = {idempotency_key.c_str(), idempotency_key.size()};
    b.error = {error.c_str(), error.size()};
    p.encode_body = agent_um_eais_task_failed_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
  } else if (type == "TASK_DONE") {
    if (task_id.empty() || step_id.empty() || idempotency_key.empty()) {
      std::fprintf(stderr, "TASK_DONE requires --task-id --step-id --idempotency-key\n");
      return 2;
    }
    encode_task_done_result_ctx_t rctx{};
    rctx.ok = result_ok ? 1 : 0;
    if (!result_text.empty()) {
      rctx.text = {result_text.c_str(), result_text.size()};
      rctx.has_text = 1;
    }
    agent_um_eais_task_done_body_t b{};
    b.task_id = {task_id.c_str(), task_id.size()};
    b.step_id = {step_id.c_str(), step_id.size()};
    b.idempotency_key = {idempotency_key.c_str(), idempotency_key.size()};
    b.encode_result = encode_task_done_result;
    b.result_ctx = &rctx;
    p.encode_body = agent_um_eais_task_done_body_encode_cbor_v0_1;
    p.body_ctx = &b;
    const int rc = encode_now();
    if (rc != 0) return rc;
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
