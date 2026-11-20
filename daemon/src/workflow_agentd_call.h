#pragma once

#include "daemon_config.h"
#include "workflow_effect.h"

#include <json/json.h>

#include <string>

namespace agentd {

// One parent-owned admission. A response loss/restart may read the original
// remote workflow, but can never authorize another submission POST.
class WorkflowAgentdCallEffect {
 public:
  WorkflowAgentdCallEffect(WorkflowEffectContext context, const Json::Value& call, bool last_attempt = true, bool previously_attempted = false);
  Json::Value preflight();
  bool needs_submission() const { return row_.effect_id.empty() || row_.state == "prepared"; }
  bool admit(std::string* error);
  Json::Value submission() const;
  const std::string& remote_key() const { return remote_key_; }
  bool matches_workflow(const Json::Value& response, std::string* workflow_id = nullptr) const;
  bool observe_workflow(const Json::Value& response, std::string* error);
  Json::Value pending(const std::string& reason, bool retryable) const;
  Json::Value complete(Json::Value result);
 private:
  void annotate(Json::Value* result, bool replayed = false) const;
  Json::Value admission() const;
  WorkflowEffectContext context_;
  AgentDb::WorkflowEffectRow definition_, row_;
  Json::Value call_;
  std::string identity_error_, remote_workflow_id_, remote_key_;
  bool last_attempt_ = true, previously_attempted_ = false;
  bool checked_ = false;
};

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
  WorkflowAgentdCallEffect& effect,
  const WorkflowEffectContext& context,
  std::string* out_err
);

}  // namespace agentd

