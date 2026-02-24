#pragma once

#include "agent/agent.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {

class AgentDb;
struct PolicyHookCtx;

struct ApprovalGateCtx {
  AgentDb* db = nullptr;
  PolicyHookCtx* hook = nullptr;
  std::unordered_set<std::string> toolset;
  std::vector<std::string> roles;
  int required = 1;
  int64_t timeout_ms = 300000;
  int64_t poll_ms = 500;
  bool enforce = false;
  bool audit = false;
  std::string trace_id;
  std::string session_id;
  std::string job_id;
  std::string team_id;
  int64_t run_id = 0;
};

agent_status_t approval_gate_tool(
  ApprovalGateCtx* gate,
  const std::string& tool_name,
  const std::string& tool_call_id,
  const std::string& arguments_json
);

}  // namespace agentd
