#pragma once

#include <json/json.h>

#include <string>

#include "agent/agent.h"

struct OpenAIClientConfig;
struct ToolLoopResult;

namespace agentd {

class AgentDb;

struct RunRequestPersistInput {
  AgentDb* db = nullptr;
  agent_session_t* session = nullptr;
  const std::string* session_id = nullptr;
  int64_t run_ts_ms = 0;
  const Json::Value* args = nullptr;
  const Json::Value* response_json = nullptr;
  bool use_tool_loop = false;
  const ToolLoopResult* tool_loop_result = nullptr;
  const std::string* trace_id = nullptr;
  const std::string* prompt = nullptr;
  const std::string* tools = nullptr;
  const OpenAIClientConfig* run_cfg = nullptr;
  bool stream_assistant = false;
  bool ok = false;
  const std::string* err = nullptr;
  long http_status = 0;
  const std::string* http_body = nullptr;
  const std::string* job_id = nullptr;
  bool yolo = false;
  const std::string* host_policy = nullptr;
  const std::string* effective_automation_profile = nullptr;
  const std::string* assistant_text = nullptr;
  const Json::Value* events_out = nullptr;
};

void persist_run_request(const RunRequestPersistInput& in);

}  // namespace agentd
