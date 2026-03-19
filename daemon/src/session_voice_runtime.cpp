#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "session_voice_backend_policy.h"
#include "session_voice_broker_client.h"
#include "session_voice_child_runtime.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_runtime_lifecycle.h"
#include "session_voice_runtime_store.h"
#include "session_voice_start_plan.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace agentd {
namespace {

static bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static std::mutex g_voice_peer_mu;
static std::unordered_map<std::string, std::shared_ptr<VoicePeerRuntime>> g_voice_peer_by_session;

static std::shared_ptr<VoicePeerRuntime> voice_peer_lookup_locked(const std::string& session_id) {
  const auto it = g_voice_peer_by_session.find(session_id);
  return it == g_voice_peer_by_session.end() ? nullptr : it->second;
}

static void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out) {
  if (!out) return;
  const Json::Value meta = session_voice_webrtc_backend_metadata_json(cfg);
  for (const auto& name : meta.getMemberNames()) {
    (*out)[name] = meta[name];
  }
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
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      st = voice_peer_lookup_locked(session_id);
      if (st) refresh_voice_peer_runtime_state(st.get());
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
      for (const auto& name : recovery_updates.getMemberNames()) {
        out[name] = recovery_updates[name];
      }
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
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
    }
    const bool was_running = st->running;
    VoicePeerStopProcessResult stop_result;
    std::string serr;
    if (!stop_voice_peer_runtime_process(st, g_voice_peer_mu, 2000, &stop_result, &serr)) {
      out["error"] = serr.empty() ? "failed to stop voice peer" : serr;
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        refresh_voice_peer_runtime_state(st.get());
        voice_peer_add_runtime_snapshot(cfg, *st, &out);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    const std::string broker_token = effective_voice_broker_token(cfg, request_broker_token);
    const Json::Value broker_cleanup = cleanup_managed_voice_peer_broker_session(st, broker_token);
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
    }
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    out["ok"] = true;
    out["stopped"] = stop_result.stopped;
    if (!was_running) out["reason"] = "not_running";
    if (broker_cleanup.isMember("broker_session_deleted")) {
      out["broker_session_deleted"] = broker_cleanup["broker_session_deleted"];
    }
    if (broker_cleanup.isMember("broker_session_delete_error")) {
      out["broker_session_delete_error"] = broker_cleanup["broker_session_delete_error"];
    }
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
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    auto st = voice_peer_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_state(st.get());
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
    if (st && !st->running) g_voice_peer_by_session.erase(session_id);
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
    for (const auto& name : recovery_updates.getMemberNames()) {
      out[name] = recovery_updates[name];
    }
    if (persisted && persisted->running) {
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        g_voice_peer_by_session[session_id] = persisted;
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
  if (start_plan.runtime_kind == "builtin") {
    out["error"] = "builtin voice_webrtc_peer runtime not implemented";
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
  }
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
  std::string serr;
  const std::string tool_path = start_plan.resolved_tool_path;
  const std::string node_bin = start_plan.resolved_node_bin;

  std::string broker_session_id = start_plan.requested_broker_session_id;
  bool managed_broker_session = false;
  if (broker_session_id.empty()) {
    if (!broker_create_audio_session(
          start_plan.effective_broker_url,
          start_plan.broker_token,
          start_plan.broker_agent_id,
          start_plan.broker_deployment_id,
          &broker_session_id,
          &serr)) {
      out["error"] = serr.empty() ? "failed to create broker audio session" : serr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    managed_broker_session = true;
  } else {
    bool session_exists = false;
    std::string broker_session_mode;
    if (!broker_audio_session_exists(
          start_plan.effective_broker_url,
          start_plan.broker_token,
          broker_session_id,
          &session_exists,
          &broker_session_mode,
          &serr)) {
      out["error"] = serr.empty() ? "failed to inspect broker audio session" : serr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (!session_exists) {
      resp->status = 400;
      resp->body = json_error_body("broker_session_id not found");
      return;
    }
    if (!broker_session_mode.empty() && broker_session_mode != "webrtc") {
      resp->status = 400;
      resp->body = json_error_body("broker_session_id mode must be webrtc");
      return;
    }
  }

  VoicePeerChildLaunchConfig launch_cfg;
  launch_cfg.runtime_kind = start_plan.runtime_kind;
  launch_cfg.session_id = session_id;
  launch_cfg.broker_session_id = broker_session_id;
  launch_cfg.broker_url = start_plan.effective_broker_url;
  launch_cfg.broker_token = start_plan.broker_token;
  launch_cfg.sender_tag = start_plan.sender_tag;
  launch_cfg.tool_path = tool_path;
  launch_cfg.node_bin = node_bin;
  launch_cfg.deadline_ms = start_plan.deadline_ms;
  launch_cfg.poll_interval_ms = start_plan.poll_interval_ms;
  launch_cfg.tone_hz = start_plan.tone_hz;

  std::shared_ptr<VoicePeerRuntime> spawned;
  if (!voice_peer_spawn_process(
        cfg,
        launch_cfg,
        g_voice_peer_mu,
        [db](const VoicePeerRuntime& persisted_state) {
          if (!db || trim_copy(persisted_state.session_id).empty()) return;
          std::string perr;
          (void)persist_voice_peer_runtime_record(db, persisted_state, &perr);
        },
        &spawned,
        &serr)) {
    if (managed_broker_session) {
      std::string derr;
      if (!broker_delete_audio_session(
            start_plan.effective_broker_url, start_plan.broker_token, broker_session_id, &derr) &&
          serr.empty()) {
        serr = derr;
      }
    }
    out["error"] = serr.empty() ? "failed to start voice peer" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  spawned->managed_broker_session = managed_broker_session;
  spawned->broker_agent_id = start_plan.broker_agent_id;
  spawned->broker_deployment_id = start_plan.broker_deployment_id;
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session[session_id] = spawned;
  }
  std::string perr;
  (void)persist_voice_peer_runtime_record(db, *spawned, &perr);
  const VoicePeerStartupWaitResult startup =
    wait_for_voice_peer_startup(spawned, g_voice_peer_mu, start_plan.startup_wait_ms);
  if (!startup.running && !startup.ready) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_session_voice_webrtc_peer_runtime(
          cfg, db, session_id, start_plan.broker_token, &cleanup, &cerr)) {
      out["error"] = cerr.empty() ? "voice peer exited before ready and cleanup failed" : cerr;
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        refresh_voice_peer_runtime_state(spawned.get());
        voice_peer_add_runtime_snapshot(cfg, *spawned, &out);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::string startup_err = trim_copy(spawned->last_error);
    if (startup_err.empty()) startup_err = "voice peer exited before ready";
    out["error"] = startup_err;
    out["startup_confirmed"] = false;
    out["startup_cleanup"] = cleanup;
    if (cleanup.isMember("broker_session_deleted")) out["broker_session_deleted"] = cleanup["broker_session_deleted"];
    if (cleanup.isMember("broker_session_delete_error")) {
      out["broker_session_delete_error"] = cleanup["broker_session_delete_error"];
    }
    out["peer"] = cleanup.isMember("peer") ? cleanup["peer"] : Json::Value(Json::nullValue);
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    refresh_voice_peer_runtime_state(spawned.get());
    voice_peer_add_runtime_snapshot(cfg, *spawned, &out);
  }
  out["ok"] = true;
  out["started"] = true;
  out["startup_confirmed"] = startup.ready;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
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
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    st = voice_peer_lookup_locked(*sid);
    if (st) refresh_voice_peer_runtime_state(st.get());
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
    for (const auto& name : recovery_updates.getMemberNames()) {
      out[name] = recovery_updates[name];
    }
    if (st && st->running) {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      g_voice_peer_by_session[*sid] = st;
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

bool cleanup_session_voice_webrtc_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  const std::string& broker_token,
  Json::Value* out_summary,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  Json::Value summary(Json::objectValue);
  summary["session_id"] = session_id;
  summary["runtime_present"] = false;
  summary["runtime_was_running"] = false;
  summary["peer"] = Json::Value(Json::nullValue);

  std::shared_ptr<VoicePeerRuntime> st;
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    st = voice_peer_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_state(st.get());
  }
  if (!st) {
    bool record_self_healed = false;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, session_id, &st, &record_self_healed, &lerr)) {
      if (out_err) *out_err = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      return false;
    }
    if (record_self_healed) summary["persisted_record_self_healed"] = true;
  }

  if (st) {
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      st->suppress_persist = true;
    }
    summary["runtime_present"] = true;
    summary["runtime_was_running"] = st->running;
    summary["peer"] = voice_peer_runtime_to_json(*st);

    VoicePeerStopProcessResult stop_result;
    if (!stop_voice_peer_runtime_process(st, g_voice_peer_mu, 2000, &stop_result, out_err)) {
      if (out_err && out_err->empty()) *out_err = "failed to stop voice peer during session delete";
      return false;
    }
    summary["stopped"] = stop_result.was_running ? stop_result.stopped : !st->running;
    summary["peer"] = voice_peer_runtime_to_json(*st);

    const Json::Value broker_cleanup =
      cleanup_managed_voice_peer_broker_session(st, effective_voice_broker_token(cfg, broker_token));
    for (const auto& name : broker_cleanup.getMemberNames()) {
      summary[name] = broker_cleanup[name];
    }
  } else {
    summary["stopped"] = false;
    summary["broker_session_delete_attempted"] = false;
  }

  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session.erase(session_id);
  }

  std::string perr;
  if (!clear_voice_peer_runtime_record(db, session_id, &perr)) {
    if (out_err) *out_err = perr.empty() ? "failed to clear persisted voice peer state" : perr;
    return false;
  }
  summary["persisted_record_cleared"] = true;

  bool artifacts_deleted = false;
  std::string aerr;
  if (!remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
    if (out_err) *out_err = aerr.empty() ? "failed to remove voice peer runtime artifacts" : aerr;
    return false;
  }
  summary["runtime_artifacts_deleted"] = artifacts_deleted;

  if (out_summary) *out_summary = std::move(summary);
  return true;
}

}  // namespace agentd
