#pragma once

#include <json/json.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace agentd {

// Expands durable workflow templates across the full task request JSON (in-place).
//
// Supported forms:
// - String templates:
//   - ${task.<id>.assistant_text}
//   - ${task.<id>.json:/ptr}
//   - ${input.<name>}
//   - ${input.<name>.json:/ptr}
// - JSON-native embedding:
//   - {"$ref":"task.<id>.assistant_text"}
//   - {"$ref":"task.<id>.json:/ptr"}
//   - {"$ref":"input.<name>"}
//   - {"$ref":"input.<name>.json:/ptr"}
//
// Semantics:
// - Pass 1 expands task.* across the whole request JSON.
// - If an "inputs" object exists, bounded rounds build inputs_by_name and expand input.* across the whole request JSON.
// - Unresolved input.* references inside the "inputs" object are deferred until the input pass (they must resolve then).
//
// Returns true if expansion succeeded; false if errors are reported (out_errors non-empty).
bool workflow_expand_templates_for_task_request(
  Json::Value* request_obj,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::vector<std::string>* out_errors
);

}  // namespace agentd

