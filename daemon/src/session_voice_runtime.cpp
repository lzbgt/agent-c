#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "session_voice_backend_policy.h"
#include "session_voice_backend_state.h"
#include "session_voice_child_backend.h"
#include "session_voice_builtin_backend.h"
#include "session_voice_broker_client.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_runtime_lifecycle.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_store.h"
#include "session_voice_start_plan.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <cstdlib>
#include <memory>

namespace agentd {
namespace {

static bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out) {
  if (!out) return;
  const Json::Value meta = session_voice_webrtc_backend_metadata_json(cfg);
  for (const auto& name : meta.getMemberNames()) {
    (*out)[name] = meta[name];
  }
}

static void merge_json_object_fields(const Json::Value& src, Json::Value* dst) {
  if (!dst || !src.isObject()) return;
  for (const auto& name : src.getMemberNames()) {
    (*dst)[name] = src[name];
  }
}

static void voice_peer_apply_start_backend_failure(
  const DaemonConfig& cfg,
  std::mutex& runtime_mu,
  const VoicePeerBackendStartResult& start_result,
  Json::Value* out
) {
  if (!out) return;
  if (start_result.state) {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_backend_state(start_result.state.get());
    voice_peer_add_runtime_snapshot(cfg, *start_result.state, out);
  }
  if (!start_result.startup_cleanup.isNull()) {
    (*out)["startup_confirmed"] = false;
    (*out)["startup_cleanup"] = start_result.startup_cleanup;
    if (start_result.startup_cleanup.isMember("broker_session_deleted")) {
      (*out)["broker_session_deleted"] = start_result.startup_cleanup["broker_session_deleted"];
    }
    if (start_result.startup_cleanup.isMember("broker_session_delete_error")) {
      (*out)["broker_session_delete_error"] = start_result.startup_cleanup["broker_session_delete_error"];
    }
    (*out)["peer"] = start_result.startup_cleanup.isMember("peer")
      ? start_result.startup_cleanup["peer"]
      : Json::Value(Json::nullValue);
  }
}

static void voice_peer_apply_start_backend_success(
  const DaemonConfig& cfg,
  std::mutex& runtime_mu,
  const VoicePeerBackendStartResult& start_result,
  Json::Value* out
) {
  if (!out || !start_result.state) return;
  std::lock_guard<std::mutex> lk(runtime_mu);
  refresh_voice_peer_runtime_backend_state(start_result.state.get());
  voice_peer_add_runtime_snapshot(cfg, *start_result.state, out);
}

}  // namespace

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
    std::shared_ptr<VoicePeerRuntime> st;
    {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      st = voice_peer_runtime_lookup_locked(session_id);
      if (st) refresh_voice_peer_runtime_backend_state(st.get());
    }
    if (!st) {
      Json::Value recovery_updates(Json::objectValue);
      std::string lerr;
    if (!recover_voice_peer_runtime_record(cfg, db, session_id, &st, &recovery_updates, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      merge_json_object_fields(recovery_updates, &out);
    }
    if (!st) {
      out["ok"] = true;
      out["stopped"] = false;
      out["reason"] = "not_running";
      out["peer"] = Json::Value(Json::nullValue);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      refresh_voice_peer_runtime_backend_state(st.get());
    }
    const bool was_running = st->running;
    VoicePeerManagedStopResult stop_result;
    std::string serr;
    if (!stop_voice_peer_runtime_with_broker_cleanup(
          st,
          voice_peer_runtime_registry_mutex(),
          2000,
          effective_voice_broker_token(cfg, request_broker_token),
          &stop_result,
          &serr)) {
      out["error"] = serr.empty() ? "failed to stop voice peer" : serr;
      {
        std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
        refresh_voice_peer_runtime_backend_state(st.get());
        voice_peer_add_runtime_snapshot(cfg, *st, &out);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
    }
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    out["ok"] = true;
    out["stopped"] = stop_result.stopped;
    if (!was_running) out["reason"] = "not_running";
    merge_json_object_fields(stop_result.broker_cleanup, &out);
    resp->status = 200;
    resp->body = json_stringify(out);
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

void handle_session_voice_webrtc_peer_status_endpoint(
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

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty() || !session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  voice_peer_add_runtime_metadata(cfg, &out);

  std::string err;
  bool session_exists = false;
  if (!db->session_exists(*sid, &session_exists, &err)) {
    resp->status = 500;
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to query session" : err;
    resp->body = json_stringify(out);
    return;
  }
  out["session_exists"] = session_exists;

  std::shared_ptr<VoicePeerRuntime> st;
  {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    st = voice_peer_runtime_lookup_locked(*sid);
    if (st) refresh_voice_peer_runtime_backend_state(st.get());
  }
  if (!st) {
    Json::Value recovery_updates(Json::objectValue);
    std::string lerr;
    if (!recover_voice_peer_runtime_record(cfg, db, *sid, &st, &recovery_updates, &lerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->body = json_stringify(out);
      return;
    }
    merge_json_object_fields(recovery_updates, &out);
    if (st && st->running) {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      voice_peer_runtime_store_locked(*sid, st);
    }
  }
  if (!session_exists && st) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_session_voice_webrtc_peer_runtime(cfg, db, *sid, "", &cleanup, &cerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = cerr.empty() ? "failed to clean up stale voice peer state" : cerr;
      if (!cleanup.empty()) out["cleanup_on_missing_session"] = cleanup;
      resp->body = json_stringify(out);
      return;
    }
    out["cleanup_on_missing_session"] = cleanup;
    st.reset();
  }
  if (st) {
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
  }
  if (st) {
    voice_peer_add_runtime_snapshot(cfg, *st, &out);
  } else {
    out["peer"] = Json::Value(Json::nullValue);
  }
  out["running"] = st ? st->running : false;
  resp->status = 200;
  resp->body = json_stringify(out);
}

}  // namespace agentd
