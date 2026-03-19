#include "session_voice_runtime_stop.h"

#include "json_util.h"
#include "session_voice_backend_state.h"
#include "session_voice_broker_client.h"
#include "session_voice_runtime_current.h"
#include "session_voice_runtime_lifecycle.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_response.h"
#include "session_voice_runtime_store.h"

namespace agentd {

void handle_session_voice_webrtc_peer_stop_action(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  const std::string& request_broker_token,
  Json::Value* out,
  HttpResponse* resp
) {
  if (!out || !resp) return;

  std::shared_ptr<VoicePeerRuntime> st;
  Json::Value recovery_updates(Json::objectValue);
  std::string lerr;
  if (!lookup_or_recover_voice_peer_runtime(cfg, db, session_id, true, &st, &recovery_updates, &lerr)) {
    (*out)["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
    resp->status = 500;
    resp->body = json_stringify(*out);
    return;
  }
  merge_json_object_fields(recovery_updates, out);

  if (!st) {
    (*out)["ok"] = true;
    (*out)["stopped"] = false;
    (*out)["reason"] = "not_running";
    (*out)["peer"] = Json::Value(Json::nullValue);
    resp->status = 200;
    resp->body = json_stringify(*out);
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
    (*out)["error"] = serr.empty() ? "failed to stop voice peer" : serr;
    {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      refresh_voice_peer_runtime_backend_state(st.get());
      voice_peer_add_runtime_snapshot(cfg, *st, out);
    }
    resp->status = 500;
    resp->body = json_stringify(*out);
    return;
  }

  {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    voice_peer_add_runtime_snapshot(cfg, *st, out);
  }
  std::string perr;
  (void)persist_voice_peer_runtime_record(db, *st, &perr);
  (*out)["ok"] = true;
  (*out)["stopped"] = stop_result.stopped;
  if (!was_running) (*out)["reason"] = "not_running";
  merge_json_object_fields(stop_result.broker_cleanup, out);
  resp->status = 200;
  resp->body = json_stringify(*out);
}

}  // namespace agentd
