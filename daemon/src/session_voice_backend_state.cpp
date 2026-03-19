#include "session_voice_backend_state.h"

#include "session_voice_child_runtime.h"

#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#endif

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

void apply_builtin_voice_peer_runtime_state(VoicePeerRuntime* st) {
  if (!st || !st->running) return;
  st->running = false;
  st->ready = false;
  if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  if (st->last_error.empty()) st->last_error = "builtin voice_webrtc_peer runtime not implemented";
}

}  // namespace

void refresh_voice_peer_runtime_backend_state(VoicePeerRuntime* st) {
  if (!st) return;
  const std::string runtime_kind = st->runtime_kind;
  if (runtime_kind == "builtin") {
    apply_builtin_voice_peer_runtime_state(st);
    return;
  }
  refresh_voice_peer_runtime_state(st);
}

bool stop_voice_peer_runtime_backend_process(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  bool was_running,
  bool* out_stopped,
  std::string* out_err
) {
  if (out_stopped) *out_stopped = false;
  if (out_err) out_err->clear();
  if (!st) {
    if (out_err) *out_err = "voice peer runtime missing";
    return false;
  }

  if (st->runtime_kind == "builtin") {
    if (was_running) {
      std::lock_guard<std::mutex> lk(runtime_mu);
      apply_builtin_voice_peer_runtime_state(st.get());
    }
    if (out_stopped) *out_stopped = was_running;
    return true;
  }

#if defined(_WIN32)
  (void)timeout_ms;
  if (st->running) {
    if (out_err) *out_err = "voice_webrtc_peer stop unsupported on Windows";
    return false;
  }
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
  if (out_stopped) *out_stopped = stopped;
  return true;
#endif
}

bool remove_voice_peer_runtime_backend_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const std::string& runtime_kind,
  bool* out_any_deleted,
  std::string* out_err
) {
  if (out_any_deleted) *out_any_deleted = false;
  if (out_err) out_err->clear();
  (void)runtime_kind;
  // Builtin has no distinct artifact store yet; keep using the shared runtime-dir cleanup
  // so stale/corrupt records continue to self-heal exactly like the shipped managed runtime.
  return remove_voice_peer_runtime_artifacts(cfg, session_id, out_any_deleted, out_err);
}

}  // namespace agentd
