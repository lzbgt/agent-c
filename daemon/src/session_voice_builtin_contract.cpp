#include "session_voice_builtin_contract.h"

#include "session_voice_broker_plan.h"
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
  const VoicePeerBrokerSessionPlan broker_session_plan =
    make_voice_peer_broker_session_plan(start_plan);
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
  out["broker_session"] = voice_peer_broker_session_plan_json(broker_session_plan);
  out["media_runtime_plan"] = voice_peer_media_runtime_plan_json(
    make_voice_peer_media_runtime_plan(
      session_id,
      start_plan,
      artifacts,
      broker_session_plan));
  out["planned_runtime"] = voice_peer_runtime_to_json(
    make_planned_voice_peer_runtime(
      session_id,
      start_plan,
      artifacts,
      broker_session_plan));
  return out;
}

}  // namespace agentd
