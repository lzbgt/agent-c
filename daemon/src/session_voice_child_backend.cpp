#include "session_voice_child_backend.h"

#include "session_voice_child_runtime.h"
#include "session_voice_launch_flow.h"
#include "session_voice_process_plan.h"
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
  VoicePeerLaunchFlowOps ops;
  ops.resolve_broker_session = resolve_voice_peer_broker_session;
  ops.launch_runtime =
    [&cfg, &runtime_mu, &persist_runtime](
      const std::string& flow_session_id,
      const VoicePeerStartPlan& flow_start_plan,
      const VoicePeerBrokerSessionBinding& binding,
      std::shared_ptr<VoicePeerRuntime>* out_state,
      std::string* out_err) {
      const VoicePeerChildLaunchConfig launch_cfg =
        make_voice_peer_child_launch_config(flow_session_id, flow_start_plan, binding);
      return voice_peer_spawn_process(
        cfg,
        launch_cfg,
        runtime_mu,
        persist_runtime,
        out_state,
        out_err);
    };
  ops.register_runtime = register_runtime;
  ops.persist_runtime = persist_runtime;
  ops.wait_startup =
    [&runtime_mu](
      const std::shared_ptr<VoicePeerRuntime>& runtime,
      int64_t timeout_ms) {
      return wait_for_voice_peer_startup(runtime, runtime_mu, timeout_ms);
    };
  ops.sync_runtime_state =
    [&runtime_mu](const std::shared_ptr<VoicePeerRuntime>& runtime) {
      std::lock_guard<std::mutex> lk(runtime_mu);
      refresh_voice_peer_runtime_state(runtime.get());
    };
  ops.cleanup_runtime = cleanup_runtime;
  ops.release_managed_broker_session = release_managed_voice_peer_broker_session;
  return run_voice_peer_launch_flow_with_ops(
    session_id, start_plan, ops, out_result);
}

}  // namespace agentd
