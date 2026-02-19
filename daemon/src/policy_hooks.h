#pragma once

#include "agent/tools.h"
#include "agent/tool_loop.h"

#include <json/json.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {

struct DaemonConfig;

enum class PolicyMode {
  Off = 0,
  Audit = 1,
  Enforce = 2,
};

const char* policy_mode_to_string(PolicyMode mode);
bool policy_mode_from_string(const std::string& s, PolicyMode* out);

struct PolicyConfig {
  PolicyMode mode = PolicyMode::Off;
  std::vector<std::string> tool_allowlist;
  std::vector<std::string> tool_denylist;
  size_t max_steps = 0;
  size_t max_tool_calls_total = 0;
  size_t max_tool_calls_per_tool = 0;
  size_t max_tool_call_args_chars = 0;
  size_t max_tool_result_chars = 0;
};

struct PolicyHookCtx {
  PolicyConfig cfg;
  std::unordered_set<std::string> allowset;
  std::unordered_set<std::string> denyset;
  Json::Value events = Json::Value(Json::arrayValue);
  std::string trace_id;
  std::string job_id;
  std::string last_tool_call_id;
  std::string last_tool_name;
  int64_t last_step = -1;
  std::string last_error;
};

struct PolicyToolExecutorCtx {
  agent_tool_executor_t base{};
  PolicyHookCtx* hook = nullptr;
};

PolicyConfig policy_config_from_daemon(const DaemonConfig& cfg);
void policy_prepare(PolicyHookCtx* ctx, const PolicyConfig& cfg, const std::string& trace_id, const std::string& job_id);

void policy_emit_start(PolicyHookCtx* ctx);
void policy_emit_complete(PolicyHookCtx* ctx, bool ok);
void policy_emit_event(PolicyHookCtx* ctx, const Json::Value& data);

void policy_apply_budget_caps(
  PolicyHookCtx* ctx,
  size_t* max_steps,
  size_t* max_tool_calls_total,
  size_t* max_tool_calls_per_tool,
  size_t* max_tool_call_args_chars,
  size_t* max_tool_result_chars
);

void policy_on_tool_loop_event(void* ctx, const char* type, const char* data_json);

agent_status_t policy_tool_execute(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
);

}  // namespace agentd
