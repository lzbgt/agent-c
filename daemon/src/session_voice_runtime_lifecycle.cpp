#include "session_voice_runtime_lifecycle.h"

#include "session_voice_broker_client.h"
#include "session_voice_child_runtime.h"
#include "string_util.h"

#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace agentd {

bool stop_voice_peer_runtime_process(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  VoicePeerStopProcessResult* out_result,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_result) *out_result = VoicePeerStopProcessResult{};
  if (!st) {
    if (out_err) *out_err = "voice peer runtime missing";
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_state(st.get());
    if (out_result) out_result->was_running = st->running;
  }
  const bool was_running = out_result ? out_result->was_running : st->running;

#if defined(_WIN32)
  if (st->running) {
    if (out_err) *out_err = "voice_webrtc_peer stop unsupported on Windows";
    return false;
  }
  if (out_result) out_result->stopped = false;
  return true;
#else
  int signal_used = 0;
  if (was_running && !voice_peer_kill_best_effort(st, runtime_mu, &signal_used, out_err)) {
    return false;
  }
  bool stopped = false;
  if (was_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopped = wait_for_voice_peer_stop(st, runtime_mu, timeout_ms);
  }
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_state(st.get());
    finalize_recovered_voice_peer_stop(st.get(), signal_used);
  }
  if (out_result) out_result->stopped = stopped;
  return true;
#endif
}

Json::Value cleanup_managed_voice_peer_broker_session(
  const std::shared_ptr<VoicePeerRuntime>& st,
  const std::string& broker_token
) {
  Json::Value summary(Json::objectValue);
  summary["broker_session_delete_attempted"] = false;
  if (!st || !st->managed_broker_session || trim_copy(st->broker_session_id).empty() || broker_token.empty()) {
    return summary;
  }

  std::string cleanup_err;
  summary["broker_session_delete_attempted"] = true;
  bool cleanup_deleted = false;
  if (validate_voice_broker_token_if_present(broker_token, &cleanup_err)) {
    cleanup_deleted = broker_delete_audio_session(st->broker_url, broker_token, st->broker_session_id, &cleanup_err);
  }
  summary["broker_session_deleted"] = cleanup_deleted;
  if (!cleanup_deleted && !cleanup_err.empty()) summary["broker_session_delete_error"] = cleanup_err;
  return summary;
}

}  // namespace agentd
