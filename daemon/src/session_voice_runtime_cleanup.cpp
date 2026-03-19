#include "session_voice_runtime_cleanup.h"

#include "session_voice_backend_state.h"
#include "session_voice_broker_client.h"
#include "session_voice_runtime_lifecycle.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_store.h"

namespace agentd {

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
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    st = voice_peer_runtime_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_backend_state(st.get());
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
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      st->suppress_persist = true;
    }
    summary["runtime_present"] = true;
    summary["runtime_was_running"] = st->running;
    summary["peer"] = voice_peer_runtime_to_json(*st);

    VoicePeerManagedStopResult stop_result;
    if (!stop_voice_peer_runtime_with_broker_cleanup(
          st,
          voice_peer_runtime_registry_mutex(),
          2000,
          effective_voice_broker_token(cfg, broker_token),
          &stop_result,
          out_err)) {
      if (out_err && out_err->empty()) *out_err = "failed to stop voice peer during session delete";
      return false;
    }
    summary["stopped"] = stop_result.was_running ? stop_result.stopped : !st->running;
    summary["peer"] = voice_peer_runtime_to_json(*st);
    for (const auto& name : stop_result.broker_cleanup.getMemberNames()) {
      summary[name] = stop_result.broker_cleanup[name];
    }
  } else {
    summary["stopped"] = false;
    summary["broker_session_delete_attempted"] = false;
  }

  {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    voice_peer_runtime_erase_locked(session_id);
  }

  std::string perr;
  if (!clear_voice_peer_runtime_record(db, session_id, &perr)) {
    if (out_err) *out_err = perr.empty() ? "failed to clear persisted voice peer state" : perr;
    return false;
  }
  summary["persisted_record_cleared"] = true;

  bool artifacts_deleted = false;
  std::string aerr;
  const std::string runtime_kind = st ? st->runtime_kind : std::string();
  if (!remove_voice_peer_runtime_backend_artifacts(
        cfg, session_id, runtime_kind, &artifacts_deleted, &aerr)) {
    if (out_err) *out_err = aerr.empty() ? "failed to remove voice peer runtime artifacts" : aerr;
    return false;
  }
  summary["runtime_artifacts_deleted"] = artifacts_deleted;

  if (out_summary) *out_summary = std::move(summary);
  return true;
}

}  // namespace agentd
