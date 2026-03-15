#pragma once

#include <json/json.h>

#include <map>
#include <string>

namespace agentd {

// Seals a JSON object as an encrypted UM-BMP body payload.
bool edge_confidentiality_seal_json_object(
  const std::string& kid,
  const std::string& secret,
  const Json::Value& body,
  Json::Value* out_body_enc,
  std::string* out_error
);

// Opens an encrypted UM-BMP body payload using the configured secret map.
bool edge_confidentiality_open_json_object(
  const Json::Value& body_enc,
  const std::map<std::string, std::string>& keys,
  Json::Value* out_body,
  std::string* out_kid,
  std::string* out_error_code,
  std::string* out_error
);

// Replaces envelope.body with envelope.body_enc using the configured kid.
bool edge_confidentiality_wrap_envelope_body(
  Json::Value* envelope_io,
  const std::map<std::string, std::string>& keys,
  const std::string& kid,
  std::string* out_error_code,
  std::string* out_error
);

// Extracts either plaintext body or decrypted body_enc from an envelope.
bool edge_confidentiality_extract_envelope_body(
  const Json::Value& envelope,
  const std::map<std::string, std::string>& keys,
  bool required,
  Json::Value* out_body,
  bool* out_used_encryption,
  std::string* out_kid,
  std::string* out_error_code,
  std::string* out_error
);

}  // namespace agentd
