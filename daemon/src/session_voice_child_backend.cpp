#include "session_voice_child_backend.h"

#include "session_voice_broker_session.h"
#include "session_voice_child_runtime.h"
#include "string_util.h"

namespace agentd {

bool start_voice_peer_child_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  std::mutex& runtime_mu,
  const std::function<void(const std::string&, const std::shared_ptr<VoicePeerRuntime>&)>& register_runtime,
  const std::function<void(const VoicePeerRuntime&)>& persist_runtime,
  const std::function<bool(const std::string&, const std::string&, Json::Value*, std::string*)>& cleanup_runtime,
  VoicePeerBackendStartResult* out_result
) {
  if (!out_result) return false;
  *out_result = VoicePeerBackendStartResult{};

  std::string serr;
  VoicePeerBrokerSessionBinding broker_binding;
  if (!resolve_voice_peer_broker_session(
        start_plan, &broker_binding, &out_result->http_status, &serr)) {
    out_result->error = serr;
    return false;
  }

  VoicePeerChildLaunchConfig launch_cfg;
  launch_cfg.runtime_kind = start_plan.runtime_kind;
  launch_cfg.session_id = session_id;
  launch_cfg.broker_session_id = broker_binding.broker_session_id;
  launch_cfg.broker_url = start_plan.effective_broker_url;
  launch_cfg.broker_token = start_plan.broker_token;
  launch_cfg.broker_agent_id = start_plan.broker_agent_id;
  launch_cfg.broker_deployment_id = start_plan.broker_deployment_id;
  launch_cfg.sender_tag = start_plan.sender_tag;
  launch_cfg.tool_path = start_plan.resolved_tool_path;
  launch_cfg.node_bin = start_plan.resolved_node_bin;
  launch_cfg.deadline_ms = start_plan.deadline_ms;
  launch_cfg.poll_interval_ms = start_plan.poll_interval_ms;
  launch_cfg.tone_hz = start_plan.tone_hz;
  launch_cfg.managed_broker_session = broker_binding.managed_broker_session;

  std::shared_ptr<VoicePeerRuntime> spawned;
  if (!voice_peer_spawn_process(
        cfg,
        launch_cfg,
        runtime_mu,
        persist_runtime,
        &spawned,
        &serr)) {
    std::string derr;
    if (!release_managed_voice_peer_broker_session(start_plan, broker_binding, &derr) &&
        serr.empty()) {
      serr = derr;
    }
    out_result->http_status = 500;
    out_result->error = serr.empty() ? "failed to start voice peer" : serr;
    return false;
  }
  if (register_runtime) register_runtime(session_id, spawned);
  if (persist_runtime) persist_runtime(*spawned);

  const VoicePeerStartupWaitResult startup =
    wait_for_voice_peer_startup(spawned, runtime_mu, start_plan.startup_wait_ms);
  if (!startup.running && !startup.ready) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_runtime || !cleanup_runtime(session_id, start_plan.broker_token, &cleanup, &cerr)) {
      out_result->http_status = 500;
      out_result->error = cerr.empty() ? "voice peer exited before ready and cleanup failed" : cerr;
      {
        std::lock_guard<std::mutex> lk(runtime_mu);
        refresh_voice_peer_runtime_state(spawned.get());
      }
      out_result->state = spawned;
      return false;
    }
    std::string startup_err = trim_copy(spawned->last_error);
    if (startup_err.empty()) startup_err = "voice peer exited before ready";
    out_result->http_status = 500;
    out_result->error = startup_err;
    out_result->startup_confirmed = false;
    out_result->startup_cleanup = cleanup;
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_state(spawned.get());
  }
  out_result->ok = true;
  out_result->http_status = 200;
  out_result->startup_confirmed = startup.ready;
  out_result->state = spawned;
  return true;
}

}  // namespace agentd
