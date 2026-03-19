#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "session_voice_backend_state.h"
#include "session_voice_child_backend.h"
#include "session_voice_builtin_backend.h"
#include "session_voice_broker_client.h"
#include "session_voice_runtime_cleanup.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_runtime_lifecycle.h"
#include "session_voice_runtime_current.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_response.h"
#include "session_voice_runtime_stop.h"
#include "session_voice_runtime_store.h"
#include "session_voice_start_plan.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <cstdlib>
#include <memory>

namespace agentd {

void handle_session_voice_webrtc_peer_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }

  const std::string session_id = body.isMember("session_id") && body["session_id"].isString() ? trim_copy(body["session_id"].asString()) : "";
  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  bool session_exists = false;
  std::string err;
  if (!db->session_exists(session_id, &session_exists, &err)) {
    resp->status = 500;
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to query session" : err;
    voice_peer_add_runtime_metadata(cfg, &out);
    resp->body = json_stringify(out);
    return;
  }
  if (!session_exists) {
    resp->status = 404;
    resp->body = json_error_body("session not found");
    return;
  }

  const std::string action = body.isMember("action") && body["action"].isString() ? lower_copy(trim_copy(body["action"].asString())) : "";
  if (action != "start" && action != "stop") {
    resp->status = 400;
    resp->body = json_error_body("action must be start or stop");
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["session_id"] = session_id;
  voice_peer_add_runtime_metadata(cfg, &out);

  const std::string request_broker_token =
    body.isMember("broker_token") && body["broker_token"].isString() ? trim_copy(body["broker_token"].asString()) : "";
  if (!request_broker_token.empty() && !is_safe_printable_field(request_broker_token, 1024)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_token");
    return;
  }

  if (action == "stop") {
    handle_session_voice_webrtc_peer_stop_action(cfg, db, session_id, request_broker_token, &out, resp);
    return;
  }

  VoicePeerStartPlan start_plan;
  VoicePeerPlanError plan_err;
  if (!build_voice_peer_start_plan(cfg, body, &start_plan, &plan_err)) {
    resp->status = plan_err.http_status;
    if (plan_err.use_json_error_body) {
      resp->body = json_error_body(plan_err.message);
    } else {
      out["error"] = plan_err.message;
      resp->body = json_stringify(out);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    auto st = voice_peer_runtime_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_backend_state(st.get());
    if (st && st->running) {
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *st, &perr);
      if (!voice_peer_runtime_matches_start_request(*st, start_plan)) {
        out["error"] = "voice peer already running with different config";
        resp->status = 409;
        resp->body = json_stringify(out);
        return;
      }
      out["ok"] = true;
      out["already_running"] = true;
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    if (st && !st->running) voice_peer_runtime_erase_locked(session_id);
  }
  {
    std::shared_ptr<VoicePeerRuntime> persisted;
    Json::Value recovery_updates(Json::objectValue);
    std::string lerr;
    if (!recover_voice_peer_runtime_record(cfg, db, session_id, &persisted, &recovery_updates, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    merge_json_object_fields(recovery_updates, &out);
    if (persisted && persisted->running) {
      {
        std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
        voice_peer_runtime_store_locked(session_id, persisted);
      }
      voice_peer_add_runtime_snapshot(cfg, *persisted, &out);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *persisted, &perr);
      if (!voice_peer_runtime_matches_start_request(*persisted, start_plan)) {
        out["error"] = "voice peer already running with different config";
        resp->status = 409;
        resp->body = json_stringify(out);
        return;
      }
      out["ok"] = true;
      out["already_running"] = true;
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
  }
  VoicePeerBackendStartResult start_result;
  if (start_plan.runtime_kind == "builtin") {
    (void)start_voice_peer_builtin_backend(cfg, session_id, start_plan, &start_result);
  } else {
    if (!finalize_voice_peer_start_plan_for_launch(cfg, request_broker_token, &start_plan, &plan_err)) {
      resp->status = plan_err.http_status;
      if (plan_err.use_json_error_body) {
        resp->body = json_error_body(plan_err.message);
      } else {
        out["error"] = plan_err.message;
        resp->body = json_stringify(out);
      }
      return;
    }

#if defined(_WIN32)
    out["error"] = "voice_webrtc_peer start unsupported on Windows";
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
#else
    (void)start_voice_peer_child_backend(
      cfg,
      session_id,
      start_plan,
      voice_peer_runtime_registry_mutex(),
      [&](const std::string& sid, const std::shared_ptr<VoicePeerRuntime>& st) {
        std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
        voice_peer_runtime_store_locked(sid, st);
      },
      [db](const VoicePeerRuntime& persisted_state) {
        if (!db || trim_copy(persisted_state.session_id).empty()) return;
        std::string perr;
        (void)persist_voice_peer_runtime_record(db, persisted_state, &perr);
      },
      [&](const std::string& sid, const std::string& broker_token, Json::Value* cleanup, std::string* cerr) {
        return cleanup_session_voice_webrtc_peer_runtime(cfg, db, sid, broker_token, cleanup, cerr);
      },
      &start_result
    );
#endif
  }

  if (!start_result.ok) {
    out["error"] = start_result.error;
    voice_peer_apply_start_backend_failure(cfg, voice_peer_runtime_registry_mutex(), start_result, &out);
    resp->status = start_result.http_status;
    if (start_result.http_status == 400 &&
        (start_result.error == "broker_session_id not found" ||
         start_result.error == "broker_session_id mode must be webrtc")) {
      resp->body = json_error_body(start_result.error);
    } else {
      resp->body = json_stringify(out);
    }
    return;
  }

  voice_peer_apply_start_backend_success(cfg, voice_peer_runtime_registry_mutex(), start_result, &out);
  out["ok"] = true;
  out["started"] = true;
  out["startup_confirmed"] = start_result.startup_confirmed;
  resp->status = 200;
  resp->body = json_stringify(out);
}

}  // namespace agentd
