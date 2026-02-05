#pragma once

#include <string>

#include <json/json.h>

namespace agentd {

// Minimal CBOR (RFC 8949) encoder for edge interop egress.
//
// Scope/constraints:
// - Encodes Json::Value into a CBOR byte string suitable for MCU/gateway consumption.
// - Uses definite lengths only.
// - JSON objects are encoded as CBOR maps with lexicographically sorted string keys for deterministic output.
// - Supported types: null, bool, int/uint (within 64-bit), double, string, array, object.
//
// Returns true on success and writes CBOR bytes into `out`.
// On failure returns false and writes a short error into `out_error` (if provided).
bool cbor_encode_json_value(
  const Json::Value& v,
  std::string* out,
  std::string* out_error
);

}  // namespace agentd

