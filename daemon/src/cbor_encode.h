#pragma once

#include <string>

#include <json/json.h>

namespace agentd {

// Minimal deterministic CBOR encoder for edge interop egress.
//
// Scope/constraints:
// - Encodes Json::Value into a CBOR byte string suitable for MCU/gateway consumption.
// - Uses definite lengths only.
// - JSON objects are encoded as CBOR maps with deterministically sorted text-string keys.
//   Ordering rule (v1):
//   - sort by UTF-8 byte length, then lexicographically by UTF-8 bytes
//   This matches the canonical ordering requirement for text-string keys in
//   RFC 8949 “Deterministically Encoded CBOR”, while keeping the implementation tiny.
// - Supported types: null, bool, int/uint (within 64-bit), double, string, array, object.
// - Doubles are always encoded as float64 (major 7 / ai 27) for determinism.
//
// Returns true on success and writes CBOR bytes into `out`.
// On failure returns false and writes a short error into `out_error` (if provided).
bool cbor_encode_json_value(
  const Json::Value& v,
  std::string* out,
  std::string* out_error
);

}  // namespace agentd
