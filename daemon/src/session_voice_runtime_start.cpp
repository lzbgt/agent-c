#include "session_voice_runtime_start.h"

#include "http_util.h"
#include "json_util.h"
#include "session_voice_builtin_backend.h"
#include "session_voice_child_backend.h"
#include "session_voice_runtime_cleanup.h"
#include "session_voice_runtime_current.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_response.h"
#include "session_voice_runtime_store.h"
#include "session_voice_start_plan.h"

#include <json/json.h>

#include <memory>

namespace agentd {

namespace {

void respond_voice_peer_start_plan_error(
  const VoicePeerPlanError& plan_err,
  Json::Value* out,
  HttpResponse* resp
) {
  resp->status = plan_err.http_status;
  if (plan_err.use_json_error_body) {
    resp->body = json_error_body(plan_err.message);
  } else {
    (*out)["error"] = plan_err.message;
    resp->body = json_stringify(*out);
  }
}

}  // namespace

void handle_session_voice_webrtc_peer_start_action(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  const std::string& session_id,
  const std::string& request_broker_token,
  Json::Value* out,
  HttpResponse* resp
) {
  VoicePeerStartPlan start_plan;
  VoicePeerPlanError plan_err;
  if (!build_voice_peer_start_plan(cfg, body, &start_plan, &plan_err)) {
    respond_voice_peer_start_plan_error(plan_err, out, resp);
    return;
  }

  std::shared_ptr<VoicePeerRuntime> st;
  Json::Value recovery_updates(Json::objectValue);
  std::string lerr;
  if (!lookup_or_recover_voice_peer_runtime(
        cfg, db, session_id, true, &st, &recovery_updates, &lerr)) {
    (*out)["error"] =
      lerr.empty() ? "failed to load persisted voice peer state" : lerr;
    resp->status = 500;
    resp->body = json_stringify(*out);
    return;
  }
  merge_json_object_fields(recovery_updates, out);

  if (st && st->running) {
    voice_peer_add_runtime_snapshot(cfg, *st, out);
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    if (!voice_peer_runtime_matches_start_request(*st, start_plan)) {
      (*out)["error"] = "voice peer already running with different config";
      resp->status = 409;
      resp->body = json_stringify(*out);
      return;
    }
    (*out)["ok"] = true;
    (*out)["already_running"] = true;
    resp->status = 200;
    resp->body = json_stringify(*out);
    return;
  }

  if (st && !st->running) {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    voice_peer_runtime_erase_locked(session_id);
  }

  VoicePeerBackendStartResult start_result;
  if (start_plan.runtime_kind == "builtin") {
    (void)start_voice_peer_builtin_backend(
      cfg, session_id, start_plan, &start_result);
  } else {
    if (!finalize_voice_peer_start_plan_for_launch(
          cfg, request_broker_token, &start_plan, &plan_err)) {
      respond_voice_peer_start_plan_error(plan_err, out, resp);
      return;
    }

#if defined(_WIN32)
    (*out)["error"] = "voice_webrtc_peer start unsupported on Windows";
    resp->status = 501;
    resp->body = json_stringify(*out);
    return;
#else
    (void)start_voice_peer_child_backend(
      cfg,
      session_id,
      start_plan,
      voice_peer_runtime_registry_mutex(),
      [&](const std::string& sid, const std::shared_ptr<VoicePeerRuntime>& runtime) {
        std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
        voice_peer_runtime_store_locked(sid, runtime);
      },
      [db](const VoicePeerRuntime& persisted_state) {
        if (!db || persisted_state.session_id.empty()) return;
        std::string perr;
        (void)persist_voice_peer_runtime_record(db, persisted_state, &perr);
      },
      [&](const std::string& sid, const std::string& broker_token, Json::Value* cleanup, std::string* cerr) {
        return cleanup_session_voice_webrtc_peer_runtime(
          cfg, db, sid, broker_token, cleanup, cerr);
      },
      &start_result
    );
#endif
  }

  if (!start_result.ok) {
    (*out)["error"] = start_result.error;
    voice_peer_apply_start_backend_failure(
      cfg, voice_peer_runtime_registry_mutex(), start_result, out);
    resp->status = start_result.http_status;
    if (start_result.http_status == 400 &&
        (start_result.error == "broker_session_id not found" ||
         start_result.error == "broker_session_id mode must be webrtc")) {
      resp->body = json_error_body(start_result.error);
    } else {
      resp->body = json_stringify(*out);
    }
    return;
  }

  voice_peer_apply_start_backend_success(
    cfg, voice_peer_runtime_registry_mutex(), start_result, out);
  (*out)["ok"] = true;
  (*out)["started"] = true;
  (*out)["startup_confirmed"] = start_result.startup_confirmed;
  resp->status = 200;
  resp->body = json_stringify(*out);
}

}  // namespace agentd
