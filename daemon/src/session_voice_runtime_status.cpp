#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "session_voice_backend_state.h"
#include "session_voice_runtime_cleanup.h"
#include "session_voice_runtime_current.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_response.h"
#include "session_voice_runtime_store.h"

namespace agentd {

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
  Json::Value recovery_updates(Json::objectValue);
  std::string lerr;
  if (!lookup_or_recover_voice_peer_runtime(cfg, db, *sid, true, &st, &recovery_updates, &lerr)) {
    resp->status = 500;
    out["ok"] = false;
    out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
    resp->body = json_stringify(out);
    return;
  }
  merge_json_object_fields(recovery_updates, &out);
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
