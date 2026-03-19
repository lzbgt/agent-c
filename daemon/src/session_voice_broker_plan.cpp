#include "session_voice_broker_plan.h"

#include "string_util.h"

namespace agentd {

VoicePeerBrokerSessionPlan make_voice_peer_broker_session_plan(
  const VoicePeerStartPlan& start_plan
) {
  VoicePeerBrokerSessionPlan plan;
  if (!trim_copy(start_plan.requested_broker_session_id).empty()) {
    plan.mode = "borrowed";
    plan.session_id = start_plan.requested_broker_session_id;
    plan.preflighted = start_plan.requested_broker_session_preflighted;
    plan.session_mode = start_plan.requested_broker_session_mode;
    plan.managed_broker_session = false;
    return plan;
  }

  plan.mode = "auto_create";
  plan.agent_id = start_plan.broker_agent_id;
  plan.deployment_id = start_plan.broker_deployment_id;
  plan.managed_broker_session = true;
  return plan;
}

Json::Value voice_peer_broker_session_plan_json(
  const VoicePeerBrokerSessionPlan& plan
) {
  Json::Value out(Json::objectValue);
  out["mode"] = plan.mode;
  out["preflighted"] = plan.preflighted;
  if (!plan.session_id.empty()) out["session_id"] = plan.session_id;
  if (!plan.session_mode.empty()) out["session_mode"] = plan.session_mode;
  if (!plan.agent_id.empty()) out["agent_id"] = plan.agent_id;
  if (!plan.deployment_id.empty()) out["deployment_id"] = plan.deployment_id;
  return out;
}

}  // namespace agentd
