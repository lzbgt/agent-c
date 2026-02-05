#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_det.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// Deterministic CBOR body encoders for node lifecycle + event messages.
//
// These are intended for MCU nodes that communicate with `agentd` using the optional
// CBOR wire profile (see `docs/spec/um-eais/um-eais-cbor-wire-profile-v0.1.md`).
//
// They are *body* encoders only (not full envelope encoders). Typical usage:
// - Build an envelope with `agent_umbmp_envelope_cbor_v0_4(...)`
// - Pass one of these as `encode_body`
//
// Determinism:
// - Uses `agent_cbor_write_map_sorted(...)` for key ordering (len, then bytes), matching the
//   platform envelope-signing profile.

typedef agent_status_t (*agent_um_eais_encode_fn)(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_um_eais_node_hello_body {
  agent_cbor_text_view_t node_id;  // required

  agent_cbor_text_view_t model;  // optional
  int has_model;

  agent_cbor_text_view_t fw_git_sha;  // optional
  int has_fw_git_sha;

  // Optional: sha256 token (64 hex or "sha256:" + 64 hex).
  agent_cbor_text_view_t caps_sha256;
  int has_caps_sha256;
} agent_um_eais_node_hello_body_t;

agent_status_t agent_um_eais_node_hello_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_um_eais_node_heartbeat_body {
  agent_cbor_text_view_t node_id;  // required

  agent_cbor_text_view_t caps_sha256;  // optional
  int has_caps_sha256;

  double battery_pct;  // optional
  int has_battery_pct;

  double rssi;  // optional
  int has_rssi;

  // Optional: encodes `health` as a CBOR map/value (deterministic rules are the caller's job).
  agent_um_eais_encode_fn encode_health;
  void* health_ctx;
} agent_um_eais_node_heartbeat_body_t;

agent_status_t agent_um_eais_node_heartbeat_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_um_eais_sensor_event_body {
  agent_cbor_text_view_t node_id;      // required
  agent_cbor_text_view_t event_type;   // required
  int64_t ts_utc_ms;                  // required

  double confidence;  // optional
  int has_confidence;

  // Required: encodes `data` as a CBOR map/value (deterministic rules are the caller's job).
  agent_um_eais_encode_fn encode_data;
  void* data_ctx;
} agent_um_eais_sensor_event_body_t;

agent_status_t agent_um_eais_sensor_event_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

