#include "session_voice_runtime_plan.h"

#include "session_voice_runtime_store.h"
#include "string_util.h"

#include <filesystem>

namespace agentd {
namespace {

std::filesystem::path voice_peer_runtime_base_dir(const DaemonConfig& cfg) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "voice_webrtc_peers";
}

}  // namespace

VoicePeerRuntimeArtifactsPlan plan_voice_peer_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id
) {
  const std::string trimmed_session_id = trim_copy(session_id);
  const std::filesystem::path runtime_dir =
    voice_peer_runtime_base_dir(cfg) / trimmed_session_id;

  VoicePeerRuntimeArtifactsPlan out;
  out.runtime_dir = runtime_dir.string();
  out.ready_file_path = (runtime_dir / "ready.json").string();
  out.stdout_log_path = (runtime_dir / "stdout.jsonl").string();
  out.stderr_log_path = (runtime_dir / "stderr.log").string();
  return out;
}

Json::Value voice_peer_runtime_artifacts_json(
  const VoicePeerRuntimeArtifactsPlan& plan
) {
  Json::Value out(Json::objectValue);
  out["runtime_dir"] = plan.runtime_dir;
  out["ready_file_path"] = plan.ready_file_path;
  out["stdout_log_path"] = plan.stdout_log_path;
  out["stderr_log_path"] = plan.stderr_log_path;
  out["ready_signal"] = "ready_file";
  out["stdout_format"] = "jsonl";
  out["stderr_format"] = "text";
  return out;
}

VoicePeerRuntime make_planned_voice_peer_runtime(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
  const std::string& broker_session_id,
  bool managed_broker_session
) {
  VoicePeerRuntime runtime;
  runtime.runtime_kind = start_plan.runtime_kind;
  runtime.status_source = "planned";
  runtime.session_id = trim_copy(session_id);
  runtime.broker_session_id = trim_copy(broker_session_id);
  runtime.broker_url = start_plan.effective_broker_url;
  runtime.managed_broker_session = managed_broker_session;
  runtime.broker_agent_id = start_plan.broker_agent_id;
  runtime.broker_deployment_id = start_plan.broker_deployment_id;
  runtime.sender_tag = start_plan.sender_tag;
  runtime.tool_path =
    start_plan.runtime_kind == "builtin" ? "@builtin" : start_plan.resolved_tool_path;
  runtime.node_bin =
    start_plan.runtime_kind == "builtin" ? "@builtin" : start_plan.resolved_node_bin;
  runtime.ready_file_path = artifacts.ready_file_path;
  runtime.stdout_log_path = artifacts.stdout_log_path;
  runtime.stderr_log_path = artifacts.stderr_log_path;
  runtime.deadline_ms = start_plan.deadline_ms;
  runtime.poll_interval_ms = start_plan.poll_interval_ms;
  runtime.tone_hz = start_plan.tone_hz;
  runtime.ready = false;
  runtime.running = false;
  return runtime;
}

VoicePeerRuntimeSeed make_spawned_voice_peer_runtime_seed(
  const VoicePeerChildLaunchConfig& launch_cfg,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
#if defined(_WIN32)
  intptr_t pid
#else
  pid_t pid
#endif
) {
  VoicePeerRuntimeSeed runtime_seed;
  runtime_seed.runtime_kind = launch_cfg.runtime_kind;
  runtime_seed.session_id = launch_cfg.session_id;
  runtime_seed.broker_session_id = launch_cfg.broker_session_id;
  runtime_seed.broker_url = launch_cfg.broker_url;
  runtime_seed.broker_agent_id = launch_cfg.broker_agent_id;
  runtime_seed.broker_deployment_id = launch_cfg.broker_deployment_id;
  runtime_seed.sender_tag = launch_cfg.sender_tag;
  runtime_seed.tool_path = launch_cfg.tool_path;
  runtime_seed.node_bin = launch_cfg.node_bin;
  runtime_seed.ready_file_path = artifacts.ready_file_path;
  runtime_seed.stdout_log_path = artifacts.stdout_log_path;
  runtime_seed.stderr_log_path = artifacts.stderr_log_path;
  runtime_seed.deadline_ms = launch_cfg.deadline_ms;
  runtime_seed.poll_interval_ms = launch_cfg.poll_interval_ms;
  runtime_seed.tone_hz = launch_cfg.tone_hz;
  runtime_seed.managed_broker_session = launch_cfg.managed_broker_session;
  runtime_seed.ready = false;
  runtime_seed.running = true;
  runtime_seed.pid = pid;
  return runtime_seed;
}

}  // namespace agentd
