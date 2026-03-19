#include "session_voice_builtin_contract.h"

#include "session_voice_launch_flow.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_runtime_store.h"

namespace agentd {

Json::Value session_voice_builtin_start_contract_json(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
) {
  const VoicePeerRuntimeArtifactsPlan artifacts =
    plan_voice_peer_runtime_artifacts(cfg, session_id);
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
  out["runtime_artifacts"] = voice_peer_runtime_artifacts_json(artifacts);

  Json::Value broker_session(Json::objectValue);
  std::string planned_broker_session_id;
  bool planned_managed_broker_session = false;
  if (!start_plan.requested_broker_session_id.empty()) {
    broker_session["mode"] = "borrowed";
    broker_session["session_id"] = start_plan.requested_broker_session_id;
    broker_session["preflighted"] = start_plan.requested_broker_session_preflighted;
    if (!start_plan.requested_broker_session_mode.empty()) {
      broker_session["session_mode"] = start_plan.requested_broker_session_mode;
    }
    planned_broker_session_id = start_plan.requested_broker_session_id;
  } else {
    broker_session["mode"] = "auto_create";
    broker_session["preflighted"] = false;
    broker_session["agent_id"] = start_plan.broker_agent_id;
    if (!start_plan.broker_deployment_id.empty()) {
      broker_session["deployment_id"] = start_plan.broker_deployment_id;
    }
    planned_managed_broker_session = true;
  }
  out["broker_session"] = broker_session;
  out["media_runtime_plan"] = voice_peer_media_runtime_plan_json(
    make_voice_peer_media_runtime_plan(
      session_id,
      start_plan,
      artifacts,
      planned_broker_session_id,
      planned_managed_broker_session));
  out["planned_runtime"] = voice_peer_runtime_to_json(
    make_planned_voice_peer_runtime(
      session_id,
      start_plan,
      artifacts,
      planned_broker_session_id,
      planned_managed_broker_session));
  return out;
}

}  // namespace agentd
