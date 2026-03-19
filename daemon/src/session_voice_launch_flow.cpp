#include "session_voice_launch_flow.h"

#include "session_voice_broker_plan.h"
#include "string_util.h"

namespace agentd {
namespace {

bool launch_flow_ops_complete(const VoicePeerLaunchFlowOps& ops) {
  return ops.resolve_broker_session && ops.launch_runtime && ops.wait_startup &&
         ops.cleanup_runtime && ops.release_managed_broker_session;
}

void append_startup_stage(
  Json::Value* out,
  const char* stage,
  bool deferred
) {
  if (!out || !stage) return;
  Json::Value row(Json::objectValue);
  row["stage"] = stage;
  row["deferred"] = deferred;
  out->append(row);
}

}  // namespace

bool run_voice_peer_launch_flow_with_ops(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerLaunchFlowOps& ops,
  VoicePeerBackendStartResult* out_result
) {
  if (!out_result) return false;
  *out_result = VoicePeerBackendStartResult{};
  if (!launch_flow_ops_complete(ops)) {
    out_result->http_status = 500;
    out_result->error = "voice peer launch flow ops incomplete";
    return false;
  }

  std::string err;
  VoicePeerBrokerSessionBinding broker_binding;
  if (!ops.resolve_broker_session(
        start_plan, &broker_binding, &out_result->http_status, &err)) {
    out_result->error = err;
    return false;
  }

  std::shared_ptr<VoicePeerRuntime> spawned;
  if (!ops.launch_runtime(
        session_id, start_plan, broker_binding, &spawned, &err)) {
    std::string release_err;
    if (!ops.release_managed_broker_session(start_plan, broker_binding, &release_err) &&
        err.empty()) {
      err = release_err;
    }
    out_result->http_status = 500;
    out_result->error = err.empty() ? "failed to start voice peer" : err;
    return false;
  }

  if (ops.register_runtime) ops.register_runtime(session_id, spawned);
  if (ops.persist_runtime && spawned) ops.persist_runtime(*spawned);

  const VoicePeerStartupWaitResult startup =
    ops.wait_startup(spawned, start_plan.startup_wait_ms);
  if (!startup.running && !startup.ready) {
    Json::Value cleanup(Json::objectValue);
    std::string cleanup_err;
    if (!ops.cleanup_runtime ||
        !ops.cleanup_runtime(session_id, start_plan.broker_token, &cleanup, &cleanup_err)) {
      out_result->http_status = 500;
      out_result->error = cleanup_err.empty()
        ? "voice peer exited before ready and cleanup failed"
        : cleanup_err;
      if (ops.sync_runtime_state && spawned) ops.sync_runtime_state(spawned);
      out_result->state = spawned;
      return false;
    }

    std::string startup_err = spawned ? trim_copy(spawned->last_error) : "";
    if (startup_err.empty()) startup_err = "voice peer exited before ready";
    out_result->http_status = 500;
    out_result->error = startup_err;
    out_result->startup_confirmed = false;
    out_result->startup_cleanup = cleanup;
    return false;
  }

  if (ops.sync_runtime_state && spawned) ops.sync_runtime_state(spawned);
  out_result->ok = true;
  out_result->http_status = 200;
  out_result->startup_confirmed = startup.ready;
  out_result->state = spawned;
  return true;
}

Json::Value voice_peer_launch_startup_sequence_json(
  const VoicePeerStartPlan& start_plan,
  bool runtime_launch_deferred
) {
  const VoicePeerBrokerSessionPlan broker_session_plan =
    make_voice_peer_broker_session_plan(start_plan);
  Json::Value out(Json::arrayValue);
  if (broker_session_plan.mode == "borrowed") {
    append_startup_stage(&out, "borrowed_broker_session_preflight", false);
  } else {
    append_startup_stage(&out, "auto_create_broker_session", true);
  }
  append_startup_stage(&out, "launch_runtime", runtime_launch_deferred);
  append_startup_stage(&out, "startup_confirmation", runtime_launch_deferred);
  append_startup_stage(&out, "startup_failure_cleanup", runtime_launch_deferred);
  return out;
}

}  // namespace agentd
