#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

// Deterministic multi-agent collaboration helper:
// - submits a workflow to a remote agentd instance
// - polls until terminal state
// - returns the remote workflow's final response JSON (best-effort bounded)
//
// Gated by cfg.workflow_enable_http_tasks (same SSRF surface as http_json).
Json::Value workflow_agentd_call_to_json(
  const DaemonConfig& cfg,
  const Json::Value& agentd_call,
  const std::string& task_trace_id,
  const std::string& previous_result_json,
  std::string* out_err
);

}  // namespace agentd

