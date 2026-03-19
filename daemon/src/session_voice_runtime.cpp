#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "session_voice_backend_policy.h"
#include "session_voice_broker_client.h"
#include "session_voice_child_runtime.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_runtime_store.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
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

static bool is_safe_shellish_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    if (!ok) return false;
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
      bool record_self_healed = false;
      std::string lerr;
      if (!load_voice_peer_runtime_record(db, session_id, &st, &record_self_healed, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      if (record_self_healed) {
        out["cleanup_on_corrupt_record"] = voice_peer_corrupt_record_cleanup_json(cfg, session_id);
      }
    }
    if (st && st->stale_persisted_record) {
      out["cleanup_on_stale_record"] = cleanup_stale_persisted_voice_peer_runtime(cfg, db, session_id);
      st.reset();
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
#if !defined(_WIN32)
    std::string serr;
    int signal_used = 0;
    if (was_running && !voice_peer_kill_best_effort(st, g_voice_peer_mu, &signal_used, &serr)) {
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
    bool stopped = false;
    if (was_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      stopped = wait_for_voice_peer_stop(st, g_voice_peer_mu, 2000);
    }
#else
    if (was_running) {
      out["error"] = "voice_webrtc_peer stop unsupported on Windows";
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
      resp->status = 501;
      resp->body = json_stringify(out);
      return;
    }
    const bool stopped = false;
#endif
    std::string cleanup_err;
    bool cleanup_attempted = false;
    bool cleanup_deleted = false;
    const std::string broker_token = effective_voice_broker_token(cfg, request_broker_token);
    if (st->managed_broker_session && !trim_copy(st->broker_session_id).empty() && !broker_token.empty()) {
      cleanup_attempted = true;
      if (validate_voice_broker_token_if_present(broker_token, &cleanup_err)) {
        cleanup_deleted = broker_delete_audio_session(st->broker_url, broker_token, st->broker_session_id, &cleanup_err);
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      finalize_recovered_voice_peer_stop(st.get(), signal_used);
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
    }
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    out["ok"] = true;
    out["stopped"] = stopped;
    if (!was_running) out["reason"] = "not_running";
    if (cleanup_attempted) {
      out["broker_session_deleted"] = cleanup_deleted;
      if (!cleanup_deleted && !cleanup_err.empty()) out["broker_session_delete_error"] = cleanup_err;
    }
    resp->status = 200;
    resp->body = json_stringify(out);
    return;
  }

  const std::string runtime_kind = body.isMember("runtime_kind") && body["runtime_kind"].isString()
    ? lower_copy(trim_copy(body["runtime_kind"].asString()))
    : default_voice_peer_runtime_kind(cfg);
  if (runtime_kind != "external" && runtime_kind != "bundled" && runtime_kind != "builtin") {
    resp->status = 400;
    resp->body = json_error_body("runtime_kind must be bundled, external, or builtin");
    return;
  }

  int64_t deadline_ms = 15000;
  if (body.isMember("deadline_ms") && (body["deadline_ms"].isInt64() || body["deadline_ms"].isUInt64() || body["deadline_ms"].isInt())) {
    deadline_ms = body["deadline_ms"].asInt64();
  }
  if (deadline_ms < 1000) deadline_ms = 1000;
  if (deadline_ms > 120000) deadline_ms = 120000;

  int64_t poll_interval_ms = 100;
  if (body.isMember("poll_interval_ms") && (body["poll_interval_ms"].isInt64() || body["poll_interval_ms"].isUInt64() || body["poll_interval_ms"].isInt())) {
    poll_interval_ms = body["poll_interval_ms"].asInt64();
  }
  if (poll_interval_ms < 25) poll_interval_ms = 25;
  if (poll_interval_ms > 5000) poll_interval_ms = 5000;

  int64_t tone_hz = 440;
  if (body.isMember("tone_hz") && (body["tone_hz"].isInt64() || body["tone_hz"].isUInt64() || body["tone_hz"].isInt())) {
    tone_hz = body["tone_hz"].asInt64();
  }
  if (tone_hz < 50) tone_hz = 50;
  if (tone_hz > 4000) tone_hz = 4000;

  int64_t startup_wait_ms = 2000;
  if (body.isMember("startup_wait_ms") &&
      (body["startup_wait_ms"].isInt64() || body["startup_wait_ms"].isUInt64() || body["startup_wait_ms"].isInt())) {
    startup_wait_ms = body["startup_wait_ms"].asInt64();
  }
  if (startup_wait_ms < 0) startup_wait_ms = 0;
  if (startup_wait_ms > 10000) startup_wait_ms = 10000;

  const std::string request_broker_url =
    body.isMember("broker_url") && body["broker_url"].isString() ? trim_copy(body["broker_url"].asString()) : "";
  const std::string requested_broker_session_id =
    body.isMember("broker_session_id") && body["broker_session_id"].isString() ? trim_copy(body["broker_session_id"].asString()) : "";
  const std::string broker_agent_id =
    body.isMember("broker_agent_id") && body["broker_agent_id"].isString() ? trim_copy(body["broker_agent_id"].asString()) : "";
  const std::string broker_deployment_id =
    body.isMember("broker_deployment_id") && body["broker_deployment_id"].isString()
      ? trim_copy(body["broker_deployment_id"].asString())
      : "";
  std::string sender_tag =
    body.isMember("sender_tag") && body["sender_tag"].isString()
      ? trim_copy(body["sender_tag"].asString())
      : std::string("agentd_runtime_peer");
  if (sender_tag.empty()) sender_tag = "agentd_runtime_peer";
  const std::string effective_broker_url = effective_voice_broker_url(cfg, request_broker_url);
  std::string desired_tool_path;
  if (runtime_kind == "bundled") desired_tool_path = discover_bundled_audio_peer_tool_path(cfg);
  if (runtime_kind == "external") desired_tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
  const std::string desired_node_bin = trim_copy(
    cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);
  std::string resolved_tool_path;
  std::string resolved_node_bin;
  std::string desired_backend_err;
  const bool desired_backend_available =
    runtime_kind != "builtin" &&
    resolve_voice_peer_backend(cfg, runtime_kind, &resolved_tool_path, &resolved_node_bin, &desired_backend_err);

  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    auto st = voice_peer_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_state(st.get());
    if (st && st->running) {
      voice_peer_add_runtime_snapshot(cfg, *st, &out);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *st, &perr);
      if (!voice_peer_runtime_matches_start_request(
            *st,
            body,
            runtime_kind,
            effective_broker_url,
            desired_tool_path,
            desired_node_bin,
            requested_broker_session_id,
            broker_agent_id,
            broker_deployment_id,
            sender_tag,
            deadline_ms,
            poll_interval_ms,
            tone_hz)) {
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
    bool record_self_healed = false;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, session_id, &persisted, &record_self_healed, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (record_self_healed) {
      out["cleanup_on_corrupt_record"] = voice_peer_corrupt_record_cleanup_json(cfg, session_id);
    }
    if (persisted && persisted->stale_persisted_record) {
      out["cleanup_on_stale_record"] = cleanup_stale_persisted_voice_peer_runtime(cfg, db, session_id);
      persisted.reset();
    }
    if (persisted && persisted->running) {
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        g_voice_peer_by_session[session_id] = persisted;
      }
      voice_peer_add_runtime_snapshot(cfg, *persisted, &out);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *persisted, &perr);
      if (!voice_peer_runtime_matches_start_request(
            *persisted,
            body,
            runtime_kind,
            effective_broker_url,
            desired_tool_path,
            desired_node_bin,
            requested_broker_session_id,
            broker_agent_id,
            broker_deployment_id,
            sender_tag,
            deadline_ms,
            poll_interval_ms,
            tone_hz)) {
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
  if (runtime_kind == "builtin") {
    out["error"] = "builtin voice_webrtc_peer runtime not implemented";
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
  }
  if (!requested_broker_session_id.empty() && !is_safe_shellish_token(requested_broker_session_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_session_id");
    return;
  }
  if (!broker_agent_id.empty() && !is_safe_shellish_token(broker_agent_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_agent_id");
    return;
  }
  if (!broker_deployment_id.empty() && !is_safe_shellish_token(broker_deployment_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_deployment_id");
    return;
  }
  if (!request_broker_url.empty() && !is_safe_printable_field(request_broker_url, 2048)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_url");
    return;
  }
  if (!requested_broker_session_id.empty() && (!broker_agent_id.empty() || !broker_deployment_id.empty())) {
    resp->status = 400;
    resp->body = json_error_body("broker_agent_id and broker_deployment_id must be omitted when broker_session_id is provided");
    return;
  }
  const std::string broker_url = effective_broker_url;
  if (broker_url.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_url required when daemon default not configured");
    return;
  }
  if (!is_safe_printable_field(broker_url, 2048)) {
    out["error"] = "invalid configured audio_webrtc_broker_url";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  const std::string broker_token = effective_voice_broker_token(cfg, request_broker_token);
  if (!validate_voice_broker_token_if_present(broker_token, &err)) {
    out["error"] = err;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  if (broker_token.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_token required when daemon default not configured");
    return;
  }
  if (requested_broker_session_id.empty() && broker_agent_id.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_agent_id required when broker_session_id omitted");
    return;
  }
  if (!is_safe_shellish_token(sender_tag, 96)) {
    resp->status = 400;
    resp->body = json_error_body("invalid sender_tag");
    return;
  }

#if defined(_WIN32)
  out["error"] = "voice_webrtc_peer start unsupported on Windows";
  resp->status = 501;
  resp->body = json_stringify(out);
  return;
#else
  std::string serr;
  std::string tool_path = resolved_tool_path;
  std::string node_bin = resolved_node_bin;
  if (!desired_backend_available) {
    serr = desired_backend_err;
  }
  if (!desired_backend_available) {
    out["error"] = serr.empty() ? "failed to resolve voice peer backend" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  std::string broker_session_id = requested_broker_session_id;
  bool managed_broker_session = false;
  if (broker_session_id.empty()) {
    if (!broker_create_audio_session(
          broker_url,
          broker_token,
          broker_agent_id,
          broker_deployment_id,
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
    if (!broker_audio_session_exists(broker_url, broker_token, broker_session_id, &session_exists, &broker_session_mode, &serr)) {
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
  launch_cfg.runtime_kind = runtime_kind;
  launch_cfg.session_id = session_id;
  launch_cfg.broker_session_id = broker_session_id;
  launch_cfg.broker_url = broker_url;
  launch_cfg.broker_token = broker_token;
  launch_cfg.sender_tag = sender_tag;
  launch_cfg.tool_path = tool_path;
  launch_cfg.node_bin = node_bin;
  launch_cfg.deadline_ms = deadline_ms;
  launch_cfg.poll_interval_ms = poll_interval_ms;
  launch_cfg.tone_hz = tone_hz;

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
      if (!broker_delete_audio_session(broker_url, broker_token, broker_session_id, &derr) && serr.empty()) serr = derr;
    }
    out["error"] = serr.empty() ? "failed to start voice peer" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  spawned->managed_broker_session = managed_broker_session;
  spawned->broker_agent_id = broker_agent_id;
  spawned->broker_deployment_id = broker_deployment_id;
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session[session_id] = spawned;
  }
  std::string perr;
  (void)persist_voice_peer_runtime_record(db, *spawned, &perr);
  const VoicePeerStartupWaitResult startup = wait_for_voice_peer_startup(spawned, g_voice_peer_mu, startup_wait_ms);
  if (!startup.running && !startup.ready) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_session_voice_webrtc_peer_runtime(cfg, db, session_id, broker_token, &cleanup, &cerr)) {
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
    bool record_self_healed = false;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, *sid, &st, &record_self_healed, &lerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->body = json_stringify(out);
      return;
    }
    if (record_self_healed) {
      out["cleanup_on_corrupt_record"] = voice_peer_corrupt_record_cleanup_json(cfg, *sid);
    }
    if (st && st->stale_persisted_record) {
      out["cleanup_on_stale_record"] = cleanup_stale_persisted_voice_peer_runtime(cfg, db, *sid);
      st.reset();
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
  }

#if defined(_WIN32)
  if (st && st->running) {
    if (out_err) *out_err = "voice_webrtc_peer cleanup unsupported on Windows";
    return false;
  }
#else
  if (st && st->running) {
    std::string serr;
    int signal_used = 0;
    if (!voice_peer_kill_best_effort(st, g_voice_peer_mu, &signal_used, &serr)) {
      if (out_err) *out_err = serr.empty() ? "failed to stop voice peer during session delete" : serr;
      return false;
    }
    summary["stopped"] = wait_for_voice_peer_stop(st, g_voice_peer_mu, 2000);
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      finalize_recovered_voice_peer_stop(st.get(), signal_used);
      summary["peer"] = voice_peer_runtime_to_json(*st);
    }
  } else {
    summary["stopped"] = st ? !st->running : false;
  }
#endif

  bool broker_deleted = false;
  const std::string broker_token_effective = effective_voice_broker_token(cfg, broker_token);
  if (st && st->managed_broker_session && !trim_copy(st->broker_session_id).empty() && !broker_token_effective.empty()) {
    std::string berr;
    summary["broker_session_delete_attempted"] = true;
    if (validate_voice_broker_token_if_present(broker_token_effective, &berr)) {
      broker_deleted = broker_delete_audio_session(st->broker_url, broker_token_effective, st->broker_session_id, &berr);
    }
    summary["broker_session_deleted"] = broker_deleted;
    if (!broker_deleted && !berr.empty()) summary["broker_session_delete_error"] = berr;
  } else {
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
