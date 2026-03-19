#include "session_voice_runtime_current.h"

#include "session_voice_backend_state.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime_store.h"

namespace agentd {

bool lookup_or_recover_voice_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  bool register_running_persisted,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_updates) *out_updates = Json::Value(Json::objectValue);

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
      if (out_err) *out_err = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      return false;
    }
    if (out_updates) *out_updates = recovery_updates;
    if (st && st->running && register_running_persisted) {
      std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
      voice_peer_runtime_store_locked(session_id, st);
    }
  }

  if (out_state) *out_state = std::move(st);
  return true;
}

}  // namespace agentd
