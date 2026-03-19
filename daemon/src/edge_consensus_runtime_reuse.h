#pragma once

#include "daemon_config.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <string>

namespace agentd {

enum class EdgeConsensusRuntimeReuseDisposition {
  not_running,
  reusable,
  invalid_request,
  conflict,
};

struct EdgeConsensusRuntimeReuseResult {
  EdgeConsensusRuntimeReuseDisposition disposition =
    EdgeConsensusRuntimeReuseDisposition::not_running;
  EdgeConsensusHttpRuntimeConfig desired_config;
  EdgeConsensusRuntime desired_state;
  Json::Value runtime = Json::Value(Json::nullValue);
  std::string error;
};

bool edge_consensus_runtime_evaluate_reuse(
  const DaemonConfig& cfg,
  const Json::Value& body,
  const std::string& runtime_kind,
  const EdgeConsensusRuntime& current,
  EdgeConsensusRuntimeReuseResult* out,
  std::string* out_err
);

}  // namespace agentd
