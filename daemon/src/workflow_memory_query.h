#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Deterministic structured memory query over checkpoints (no LLM required).
//
// Scans `${state_dir}/memory/checkpoints/structured_*.json` and selects the newest checkpoint in the
// requested time window (optionally filtered by `structured_path`). Returns keys whose names match
// `key_prefix` (if provided), ordered deterministically.
//
// The output shape matches the workflow task result `kind:"memory_query"` (except the `kind` field,
// which the caller may attach).
Json::Value workflow_memory_query_to_json(
  const std::string& state_dir,
  const Json::Value& memory_query,
  std::string* out_err
);

}  // namespace agentd

