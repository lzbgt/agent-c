#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Deterministic correlation query over structured memory checkpoints.
//
// Scans `${state_dir}/memory/checkpoints/structured_*.json` and returns entries whose
// `record.sources[]` contains a substring `trace:<trace_id>`, where:
// - `trace_id` comes from `memory_correlate.trace_id` if provided; else `workflow_trace_id`.
//
// The output shape matches the workflow task result `kind:"memory_correlate"` (except the `kind` field,
// which the caller may attach).
Json::Value workflow_memory_correlate_to_json(
  const std::string& state_dir,
  const std::string& workflow_trace_id,
  const Json::Value& memory_correlate,
  std::string* out_err
);

}  // namespace agentd

