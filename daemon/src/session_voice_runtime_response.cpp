#include "session_voice_runtime_response.h"

#include "session_voice_backend_policy.h"
#include "session_voice_backend_state.h"
#include "session_voice_runtime_store.h"

namespace agentd {

bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out) {
  if (!out) return;
  const Json::Value meta = session_voice_webrtc_backend_metadata_json(cfg);
  for (const auto& name : meta.getMemberNames()) {
    (*out)[name] = meta[name];
  }
}

void merge_json_object_fields(const Json::Value& src, Json::Value* dst) {
  if (!dst || !src.isObject()) return;
  for (const auto& name : src.getMemberNames()) {
    (*dst)[name] = src[name];
  }
}

void voice_peer_apply_start_backend_failure(
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
  merge_json_object_fields(start_result.backend_info, out);
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

void voice_peer_apply_start_backend_success(
  const DaemonConfig& cfg,
  std::mutex& runtime_mu,
  const VoicePeerBackendStartResult& start_result,
  Json::Value* out
) {
  if (!out || !start_result.state) return;
  std::lock_guard<std::mutex> lk(runtime_mu);
  refresh_voice_peer_runtime_backend_state(start_result.state.get());
  voice_peer_add_runtime_snapshot(cfg, *start_result.state, out);
  merge_json_object_fields(start_result.backend_info, out);
}

}  // namespace agentd
