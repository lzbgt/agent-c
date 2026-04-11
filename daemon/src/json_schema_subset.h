#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Best-effort JSON Schema subset validator for deterministic runtime checks.
// Supported keywords: type, enum, required, properties, additionalProperties:false, items.
// Unknown/unsupported keywords are ignored.
bool json_schema_subset_validate_best_effort(
  const Json::Value& schema,
  const Json::Value& value,
  const std::string& path,
  std::string* out_error
);

}  // namespace agentd
