#include "session_voice_builtin_backend.h"

#include "session_voice_backend_policy.h"
#include "session_voice_broker_plan.h"
#include "session_voice_broker_session.h"
#include "session_voice_builtin_contract.h"
#include "session_voice_builtin_service.h"
#include "session_voice_launch_flow.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_runtime_registry.h"
#include "string_util.h"

#include <memory>
#include <mutex>

namespace agentd {

bool start_voice_peer_builtin_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  VoicePeerBackendStartResult* out_result
) {
  if (!out_result) return false;
  const std::string unavailable_reason =
    voice_peer_backend_unavailable_reason(cfg, start_plan.runtime_kind);
  const VoicePeerBuiltinStartPreview preview =
    build_voice_peer_builtin_start_preview(cfg, session_id, start_plan);
  if (!trim_copy(unavailable_reason).empty()) {
    *out_result = VoicePeerBackendStartResult{};
    out_result->http_status = 501;
    out_result->error = unavailable_reason;
    out_result->state = std::make_shared<VoicePeerRuntime>(preview.planned_runtime);
    out_result->backend_info["builtin_start_contract"] = preview.contract;
    return false;
  }

  VoicePeerLaunchFlowOps ops;
  ops.resolve_broker_session = resolve_voice_peer_broker_session;
  ops.launch_runtime =
    [&cfg](
      const std::string& flow_session_id,
      const VoicePeerStartPlan& flow_start_plan,
      const VoicePeerBrokerSessionBinding& binding,
      std::shared_ptr<VoicePeerRuntime>* out_state,
      std::string* out_err) {
      return start_builtin_voice_peer_runtime_service(
        cfg,
        flow_session_id,
        flow_start_plan,
        binding,
        voice_peer_runtime_registry_mutex(),
        [](const VoicePeerRuntime&) {},
        out_state,
        out_err);
    };
  ops.register_runtime =
    [](const std::string& flow_session_id, const std::shared_ptr<VoicePeerRuntime>& runtime) {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      voice_peer_runtime_store_locked(flow_session_id, runtime);
    };
  ops.wait_startup =
    [](const std::shared_ptr<VoicePeerRuntime>& runtime, int64_t) {
      VoicePeerStartupWaitResult result;
      if (runtime) {
        result.ready = runtime->ready;
        result.running = runtime->running;
      }
      return result;
    };
  ops.sync_runtime_state =
    [](const std::shared_ptr<VoicePeerRuntime>& runtime) {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      if (runtime) refresh_builtin_voice_peer_runtime_state(runtime.get());
    };
  ops.cleanup_runtime =
    [](const std::string&, const std::string&, Json::Value*, std::string*) {
      return true;
    };
  ops.release_managed_broker_session = release_managed_voice_peer_broker_session;

  const bool ok = run_voice_peer_launch_flow_with_ops(session_id, start_plan, ops, out_result);
  if (!out_result->backend_info.isObject()) out_result->backend_info = Json::Value(Json::objectValue);
  out_result->backend_info["builtin_start_contract"] = preview.contract;
  return ok;
}

}  // namespace agentd
