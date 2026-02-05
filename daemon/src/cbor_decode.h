#pragma once

#include <string>

#include <json/json.h>

namespace agentd {

// Minimal CBOR (RFC 8949) decoder for edge interop ingress.
//
// Scope/constraints:
// - Supports the subset needed for UM-BMP envelopes: maps/arrays, text strings, ints, bool, null, floats.
// - Requires definite lengths (indefinite-length items are rejected) to keep decoding bounded and deterministic.
// - Map keys must be text strings (CBOR major type 3), which matches the spec's JSON-shaped envelope.
// - Tags (major type 6) are ignored and the tagged item is decoded.
//
// Returns true on success and writes a Json::Value tree into `out`.
// On failure returns false and writes a short error into `out_error` (if provided).
bool cbor_decode_to_json_value(
  const std::string& cbor_bytes,
  Json::Value* out,
  std::string* out_error
);

}  // namespace agentd

