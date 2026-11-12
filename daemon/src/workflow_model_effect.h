#pragma once
#include "workflow_effect.h"
#include "agent/tool_provider.h"
#include <map>
#include <functional>

struct ToolLoopOptions;
struct OpenAIToolProviderCtx;
namespace agentd {

// Workflow-owned effect guard. Host-model runs may checkpoint and replay exact
// provider/tool records. Persistent conversations use durable revision fences.
class WorkflowModelEffect {
 public:
  WorkflowModelEffect(WorkflowEffectContext context, const std::string& task_request_json);
  Json::Value preflight();
  using Authorize = std::function<agent_status_t(const char*, const char*)>;
  agent_tool_executor_t wrap(agent_tool_executor_t base, Authorize authorize = {});
  void bind_scope(const Json::Value& scope);
  Json::Value bind_session(const std::string& session_id, int64_t revision);
  void stage_session(std::vector<AgentDb::MessageRow> messages, const std::string& error = {});
  bool session_committed() const { return session_committed_; }
  bool finished() const { return finished_; }
  void configure_transcript(ToolLoopOptions* options, const Json::Value& provider_identity);
  void set_fresh_budget(bool tools_limited, int64_t tools, bool steps_limited, int64_t steps);
  bool has_checkpoint() const { return restored_; }
  agent_tool_provider_t wrap_provider(agent_tool_provider_t base, OpenAIToolProviderCtx* context = nullptr);
  Json::Value finish(Json::Value result);

 private:
  static agent_status_t execute(void*, const char*, const char*, agent_string_t*);
  static agent_status_t generate(void*, const agent_tool_provider_request_t*, agent_tool_provider_response_t*);
  Json::Value failure(const std::string& reason, bool retryable = false) const;
  Json::Value unresolved(const std::string& reason) const;
  void annotate(Json::Value* result, bool replayed = false) const;
  bool cancelled() const;
  bool validate_session();
  Json::Value checkpoint_value() const;
  bool save_checkpoint(bool admitting_tool);
  bool append_record(Json::Value record);
  bool restore_checkpoint(const Json::Value& value);
  agent_status_t reject(const std::string& reason);
  WorkflowEffectContext context_;
  AgentDb::WorkflowEffectRow definition_, row_;
  agent_tool_executor_t base_{};
  agent_tool_provider_t provider_{};
  OpenAIToolProviderCtx* provider_context_ = nullptr;
  Authorize authorize_;
  AgentDb::SessionFence session_;
  std::vector<AgentDb::MessageRow> session_messages_;
  bool session_bound_ = false, session_configured_ = false, session_staged_ = false;
  bool session_conflict_ = false, session_committed_ = false, finished_ = false;
  bool checked_ = false, dispatched_ = false;
  bool transcript_ = false, restored_ = false, provider_failed_ = false, budget_refused_ = false;
  bool tools_budget_limited_ = false, steps_budget_limited_ = false;
  int64_t tools_budget_ = 0, steps_budget_ = 0;
  std::string identity_error_, uncertain_, scope_hash_, config_hash_;
  Json::Value refusal_;
  Json::Value calls_by_tool_{Json::objectValue};
  int64_t calls_ = 0, provider_calls_ = 0;
  size_t provider_limit_ = 0, tool_limit_ = 0, per_tool_limit_ = 0;
  bool provider_limited_ = false, tool_limited_ = false;
  std::map<std::string, size_t> per_tool_limits_;
  Json::Value records_{Json::arrayValue}, pending_;
  size_t cursor_ = 0, replayed_providers_ = 0, replayed_tools_ = 0;
};
}  // namespace agentd
