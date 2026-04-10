#include "session_voice_builtin_contract.h"

#include "session_voice_backend_policy.h"
#include "session_voice_broker_plan.h"
#include "session_voice_launch_flow.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_runtime_store.h"
#include "string_util.h"

namespace agentd {

VoicePeerBuiltinStartPreview build_voice_peer_builtin_start_preview(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
) {
  const VoicePeerRuntimeArtifactsPlan artifacts =
    plan_voice_peer_runtime_artifacts(cfg, session_id);
  const VoicePeerBrokerSessionPlan broker_session_plan =
    make_voice_peer_broker_session_plan(start_plan);
  const Json::Value startup_sequence =
    voice_peer_launch_startup_sequence_json(start_plan, !builtin_voice_peer_runtime_enabled(cfg));
  const Json::Value runtime_artifacts = voice_peer_runtime_artifacts_json(artifacts);
  const Json::Value broker_session = voice_peer_broker_session_plan_json(broker_session_plan);
  const Json::Value media_runtime_plan = voice_peer_media_runtime_plan_json(
    make_voice_peer_media_runtime_plan(
      session_id,
      start_plan,
      artifacts,
      broker_session_plan));
  VoicePeerBuiltinStartPreview out;
  out.planned_runtime = make_planned_voice_peer_runtime(
    session_id,
    start_plan,
    artifacts,
    broker_session_plan);
  out.planned_runtime.last_error =
    voice_peer_backend_unavailable_reason(cfg, start_plan.runtime_kind);
  out.contract["session_id"] = session_id;
  out.contract["runtime_kind"] = "builtin";
  out.contract["signaling_surface"] = "voice_webrtc_peer";
  out.contract["broker_url"] = start_plan.effective_broker_url;
  out.contract["sender_tag"] = start_plan.sender_tag;
  out.contract["deadline_ms"] = Json::Int64(start_plan.deadline_ms);
  out.contract["poll_interval_ms"] = Json::Int64(start_plan.poll_interval_ms);
  out.contract["tone_hz"] = Json::Int64(start_plan.tone_hz);
  out.contract["startup_wait_ms"] = Json::Int64(start_plan.startup_wait_ms);
  out.contract["mutating_broker_actions_deferred"] = !builtin_voice_peer_runtime_enabled(cfg);
  out.contract["startup_sequence"] = startup_sequence;
  out.contract["runtime_artifacts"] = runtime_artifacts;
  out.contract["broker_session"] = broker_session;
  out.contract["media_runtime_plan"] = media_runtime_plan;
  out.contract["planned_runtime"] = voice_peer_runtime_to_json(out.planned_runtime);
  return out;
}

Json::Value session_voice_builtin_start_contract_json(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
) {
  return build_voice_peer_builtin_start_preview(cfg, session_id, start_plan).contract;
}

}  // namespace agentd
