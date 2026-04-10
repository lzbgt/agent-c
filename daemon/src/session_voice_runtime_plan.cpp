#include "session_voice_runtime_plan.h"

#include "session_voice_builtin_media_engine.h"
#include "session_voice_process_plan.h"
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

void apply_media_runtime_plan_fields(
  const VoicePeerMediaRuntimePlan& media_plan,
  VoicePeerRuntime* runtime
) {
  if (!runtime) return;
  runtime->runtime_kind = media_plan.runtime_kind;
  runtime->session_id = media_plan.session_id;
  runtime->broker_session_id = media_plan.broker_session_id;
  runtime->broker_url = media_plan.broker_url;
  runtime->managed_broker_session = media_plan.managed_broker_session;
  runtime->broker_agent_id = media_plan.broker_agent_id;
  runtime->broker_deployment_id = media_plan.broker_deployment_id;
  runtime->sender_tag = media_plan.sender_tag;
  runtime->ready_file_path = media_plan.ready_file_path;
  runtime->deadline_ms = media_plan.deadline_ms;
  runtime->poll_interval_ms = media_plan.poll_interval_ms;
  runtime->tone_hz = media_plan.tone_hz;
}

void apply_media_runtime_plan_fields(
  const VoicePeerMediaRuntimePlan& media_plan,
  VoicePeerRuntimeSeed* seed
) {
  if (!seed) return;
  seed->runtime_kind = media_plan.runtime_kind;
  seed->session_id = media_plan.session_id;
  seed->broker_session_id = media_plan.broker_session_id;
  seed->broker_url = media_plan.broker_url;
  seed->managed_broker_session = media_plan.managed_broker_session;
  seed->broker_agent_id = media_plan.broker_agent_id;
  seed->broker_deployment_id = media_plan.broker_deployment_id;
  seed->sender_tag = media_plan.sender_tag;
  seed->ready_file_path = media_plan.ready_file_path;
  seed->deadline_ms = media_plan.deadline_ms;
  seed->poll_interval_ms = media_plan.poll_interval_ms;
  seed->tone_hz = media_plan.tone_hz;
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
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
  const VoicePeerBrokerSessionPlan& broker_session_plan
) {
  const VoicePeerMediaRuntimePlan media_plan = make_voice_peer_media_runtime_plan(
    session_id, start_plan, artifacts, broker_session_plan);
  VoicePeerRuntime runtime;
  apply_media_runtime_plan_fields(media_plan, &runtime);
  apply_voice_peer_media_engine_info(
    voice_peer_media_engine_info_for_runtime_kind(cfg, start_plan.runtime_kind),
    &runtime);
  runtime.status_source = "planned";
  runtime.tool_path =
    start_plan.runtime_kind == "builtin" ? "@builtin" : start_plan.resolved_tool_path;
  runtime.node_bin =
    start_plan.runtime_kind == "builtin" ? "@builtin" : start_plan.resolved_node_bin;
  runtime.stdout_log_path = artifacts.stdout_log_path;
  runtime.stderr_log_path = artifacts.stderr_log_path;
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
  const VoicePeerChildProcessPlan process_plan =
    make_voice_peer_child_process_plan(launch_cfg, artifacts);
  VoicePeerRuntimeSeed runtime_seed;
  apply_media_runtime_plan_fields(process_plan.media_runtime_plan, &runtime_seed);
  runtime_seed.tool_path = launch_cfg.tool_path;
  runtime_seed.node_bin = launch_cfg.node_bin;
  runtime_seed.stdout_log_path = artifacts.stdout_log_path;
  runtime_seed.stderr_log_path = artifacts.stderr_log_path;
  runtime_seed.ready = false;
  runtime_seed.running = true;
  runtime_seed.pid = pid;
  apply_voice_peer_media_engine_info(
    voice_peer_media_engine_info_for_runtime_kind(DaemonConfig{}, launch_cfg.runtime_kind),
    &runtime_seed);
  return runtime_seed;
}

}  // namespace agentd
