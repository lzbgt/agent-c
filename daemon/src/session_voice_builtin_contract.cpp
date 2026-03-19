#include "session_voice_builtin_contract.h"

#include "session_voice_launch_flow.h"

namespace agentd {

Json::Value session_voice_builtin_start_contract_json(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
) {
  Json::Value out(Json::objectValue);
  out["session_id"] = session_id;
  out["runtime_kind"] = "builtin";
  out["signaling_surface"] = "voice_webrtc_peer";
  out["broker_url"] = start_plan.effective_broker_url;
  out["sender_tag"] = start_plan.sender_tag;
  out["deadline_ms"] = Json::Int64(start_plan.deadline_ms);
  out["poll_interval_ms"] = Json::Int64(start_plan.poll_interval_ms);
  out["tone_hz"] = Json::Int64(start_plan.tone_hz);
  out["startup_wait_ms"] = Json::Int64(start_plan.startup_wait_ms);
  out["mutating_broker_actions_deferred"] = true;
  out["startup_sequence"] =
    voice_peer_launch_startup_sequence_json(start_plan, true);

  Json::Value broker_session(Json::objectValue);
  if (!start_plan.requested_broker_session_id.empty()) {
    broker_session["mode"] = "borrowed";
    broker_session["session_id"] = start_plan.requested_broker_session_id;
    broker_session["preflighted"] = start_plan.requested_broker_session_preflighted;
    if (!start_plan.requested_broker_session_mode.empty()) {
      broker_session["session_mode"] = start_plan.requested_broker_session_mode;
    }
  } else {
    broker_session["mode"] = "auto_create";
    broker_session["preflighted"] = false;
    broker_session["agent_id"] = start_plan.broker_agent_id;
    if (!start_plan.broker_deployment_id.empty()) {
      broker_session["deployment_id"] = start_plan.broker_deployment_id;
    }
  }
  out["broker_session"] = broker_session;
  return out;
}

}  // namespace agentd
