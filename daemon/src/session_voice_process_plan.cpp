#include "session_voice_process_plan.h"

#include "string_util.h"

namespace agentd {

VoicePeerMediaRuntimePlan make_voice_peer_media_runtime_plan(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
  const std::string& broker_session_id,
  bool managed_broker_session
) {
  VoicePeerMediaRuntimePlan plan;
  plan.runtime_kind = start_plan.runtime_kind;
  plan.session_id = trim_copy(session_id);
  plan.broker_session_id = trim_copy(broker_session_id);
  plan.broker_url = start_plan.effective_broker_url;
  plan.managed_broker_session = managed_broker_session;
  plan.broker_agent_id = start_plan.broker_agent_id;
  plan.broker_deployment_id = start_plan.broker_deployment_id;
  plan.sender_tag = start_plan.sender_tag;
  plan.ready_file_path = artifacts.ready_file_path;
  plan.deadline_ms = start_plan.deadline_ms;
  plan.poll_interval_ms = start_plan.poll_interval_ms;
  plan.tone_hz = start_plan.tone_hz;
  return plan;
}

Json::Value voice_peer_media_runtime_plan_json(
  const VoicePeerMediaRuntimePlan& plan
) {
  Json::Value out(Json::objectValue);
  out["schema"] = plan.schema;
  out["signaling_surface"] = plan.signaling_surface;
  out["runtime_kind"] = plan.runtime_kind;
  out["session_id"] = plan.session_id;
  if (!plan.broker_session_id.empty()) out["broker_session_id"] = plan.broker_session_id;
  out["broker_url"] = plan.broker_url;
  out["managed_broker_session"] = plan.managed_broker_session;
  if (!plan.broker_agent_id.empty()) out["broker_agent_id"] = plan.broker_agent_id;
  if (!plan.broker_deployment_id.empty()) out["broker_deployment_id"] = plan.broker_deployment_id;
  out["sender_tag"] = plan.sender_tag;
  out["ready_signal"] = "ready_file";
  out["ready_file_path"] = plan.ready_file_path;
  out["deadline_ms"] = Json::Int64(plan.deadline_ms);
  out["poll_interval_ms"] = Json::Int64(plan.poll_interval_ms);
  out["tone_hz"] = Json::Int64(plan.tone_hz);
  return out;
}

VoicePeerChildProcessPlan make_voice_peer_child_process_plan(
  const VoicePeerChildLaunchConfig& launch_cfg,
  const VoicePeerRuntimeArtifactsPlan& artifacts
) {
  VoicePeerChildProcessPlan plan;
  plan.media_runtime_plan.schema = "voice_webrtc_peer_media_runtime_plan_v1";
  plan.media_runtime_plan.signaling_surface = "voice_webrtc_peer";
  plan.media_runtime_plan.runtime_kind = launch_cfg.runtime_kind;
  plan.media_runtime_plan.session_id = launch_cfg.session_id;
  plan.media_runtime_plan.broker_session_id = launch_cfg.broker_session_id;
  plan.media_runtime_plan.broker_url = launch_cfg.broker_url;
  plan.media_runtime_plan.managed_broker_session = launch_cfg.managed_broker_session;
  plan.media_runtime_plan.broker_agent_id = launch_cfg.broker_agent_id;
  plan.media_runtime_plan.broker_deployment_id = launch_cfg.broker_deployment_id;
  plan.media_runtime_plan.sender_tag = launch_cfg.sender_tag;
  plan.media_runtime_plan.ready_file_path = artifacts.ready_file_path;
  plan.media_runtime_plan.deadline_ms = launch_cfg.deadline_ms;
  plan.media_runtime_plan.poll_interval_ms = launch_cfg.poll_interval_ms;
  plan.media_runtime_plan.tone_hz = launch_cfg.tone_hz;

  const std::string deadline_s = std::to_string((long long)launch_cfg.deadline_ms);
  const std::string poll_s = std::to_string((long long)launch_cfg.poll_interval_ms);
  const std::string tone_s = std::to_string((long long)launch_cfg.tone_hz);

  plan.argv.push_back(launch_cfg.node_bin);
  plan.argv.push_back(launch_cfg.tool_path);
  plan.argv.push_back("--broker-url");
  plan.argv.push_back(launch_cfg.broker_url);
  plan.argv.push_back("--token");
  plan.argv.push_back(launch_cfg.broker_token);
  plan.argv.push_back("--session-id");
  plan.argv.push_back(launch_cfg.broker_session_id);
  plan.argv.push_back("--ready-file");
  plan.argv.push_back(artifacts.ready_file_path);
  plan.argv.push_back("--deadline-ms");
  plan.argv.push_back(deadline_s);
  plan.argv.push_back("--poll-interval-ms");
  plan.argv.push_back(poll_s);
  plan.argv.push_back("--tone-hz");
  plan.argv.push_back(tone_s);
  if (!launch_cfg.sender_tag.empty()) {
    plan.argv.push_back("--sender-tag");
    plan.argv.push_back(launch_cfg.sender_tag);
  }
  return plan;
}

}  // namespace agentd
