#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Helpers used only by workflow submit endpoint to validate/sanitize deterministic task specs
// into the canonical workflow task request JSON persisted in the DB.

bool workflow_submit_build_http_json_task_request(
  const Json::Value& task_spec,
  const std::string& task_id,
  int task_priority,
  const std::string& trace_id,
  Json::Value* out_task_req,
  std::string* out_error
);

bool workflow_submit_build_agentd_call_task_request(
  const Json::Value& task_spec,
  const std::string& task_id,
  int task_priority,
  const std::string& trace_id,
  Json::Value* out_task_req,
  std::string* out_error
);

}  // namespace agentd

