#pragma once

#include <json/json.h>

#include <atomic>
#include <string>
#include <vector>

#include "run_endpoints.h"
#include "run_memory_context.h"

#include "tool_loop.h"

struct OpenAIClientConfig;

namespace agentd {

struct DaemonConfig;
struct PolicyHookCtx;

struct RunRequestToolLoopInput {
  const DaemonConfig* daemon_cfg = nullptr;
  const Json::Value* args = nullptr;
  const OpenAIClientConfig* run_cfg = nullptr;
  const std::string* prompt = nullptr;
  const std::string* prompt_for_llm = nullptr;
  const std::string* trace_id = nullptr;
  const std::string* session_id = nullptr;
  const std::string* tools = nullptr;
  bool no_session = false;
  bool no_default_system = false;
  const MemoryContextPolicy* mem_pol = nullptr;
  const std::string* mem_query = nullptr;
  size_t max_steps = 0;
  size_t max_tool_calls_total = 0;
  size_t max_tool_calls_per_tool = 0;
  size_t max_tool_call_args_chars = 0;
  size_t max_tool_result_chars = 0;
  const std::vector<ToolCallLimit>* tool_call_limits = nullptr;
  size_t max_capture_bytes = 0;
  size_t max_chars = 0;
  size_t keep_last = 0;
  bool verbose = false;
  bool stream_assistant = false;
  agent_session_t* session = nullptr;
  agent_tool_registry_t* registry = nullptr;
  agent_tool_executor_t* executor = nullptr;
  std::ostream* trace_stream = nullptr;
  RunCancelCallback should_cancel_or_null = nullptr;
  void* should_cancel_ctx_or_null = nullptr;
  const std::string* job_id = nullptr;
  PolicyHookCtx* policy_hook = nullptr;
  std::atomic<int64_t>* heartbeat_last_any_event_ms = nullptr;
  std::atomic<int64_t>* heartbeat_last_non_ms = nullptr;
  std::atomic<int>* heartbeat_phase = nullptr;
};

struct RunRequestToolLoopResult {
  bool ok = false;
  std::string assistant_text;
  std::string err;
  long http_status = 0;
  std::string http_body;
  Json::Value events_out;
  ToolLoopResult tool_loop_result;
  bool vision_prefetch_attempted = false;
  bool vision_prefetch_ok = false;
};

RunRequestToolLoopResult run_request_tool_loop(const RunRequestToolLoopInput& in);

}  // namespace agentd
