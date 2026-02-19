#pragma once

#include <string>

namespace Json { class Value; }
struct ToolLoopResult;

namespace agentd {

struct RunReplayBundle {
  std::string request_json;
  std::string response_json;
  std::string sha256;
  std::string sha256_alg;
  std::string sha256_schema;
  std::string error;
};

RunReplayBundle build_run_replay_bundle(
  const Json::Value& request_args,
  const Json::Value& response,
  const ToolLoopResult* tool_loop_result,
  const std::string& trace_id
);

}  // namespace agentd
