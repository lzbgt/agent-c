#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

// Deterministic outbound HTTP JSON task for workflows.
//
// Input: `http_json` spec object from the workflow task request.
// Output: JSON object compatible with a workflow task result (excluding the `kind` field).
Json::Value workflow_http_json_to_json(
  const DaemonConfig& cfg,
  const Json::Value& http_json,
  std::string* out_err
);

}  // namespace agentd

