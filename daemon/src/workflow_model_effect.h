#pragma once
#include "workflow_effect.h"
#include "agent/tools.h"

namespace agentd {

// One conservative boundary around a model-selected host-tool run, not a
// transcript checkpoint. Only the workflow engine supplies this context.
class WorkflowModelEffect {
 public:
  WorkflowModelEffect(WorkflowEffectContext context, const std::string& task_request_json);
  // Null means continue. A saved receipt or unresolved dispatch needs no new
  // provider/tool budget. This read never creates an effect for pure tasks.
  Json::Value preflight();
  agent_tool_executor_t wrap(agent_tool_executor_t base);
  Json::Value finish(Json::Value result);

 private:
  static agent_status_t execute(void*, const char*, const char*, agent_string_t*);
  Json::Value failure(const std::string& reason, bool retryable = false) const;
  Json::Value unresolved(const std::string& reason) const;
  void annotate(Json::Value* result, bool replayed = false) const;
  bool cancelled() const;
  WorkflowEffectContext context_;
  AgentDb::WorkflowEffectRow definition_, row_;
  agent_tool_executor_t base_{};
  bool checked_ = false;
  bool dispatched_ = false;
  std::string identity_error_, uncertain_;
  Json::Value refusal_;
  Json::Value calls_by_tool_{Json::objectValue};
  int64_t calls_ = 0;
};
}  // namespace agentd
