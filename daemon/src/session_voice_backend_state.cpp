#include "session_voice_backend_state.h"

#include "session_voice_child_runtime.h"

#include <chrono>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

}  // namespace

void refresh_voice_peer_runtime_backend_state(VoicePeerRuntime* st) {
  if (!st) return;
  const std::string runtime_kind = st->runtime_kind;
  if (runtime_kind == "builtin") {
    if (st->running) {
      st->running = false;
      st->ready = false;
      if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
      if (st->last_error.empty()) st->last_error = "builtin voice_webrtc_peer runtime not implemented";
    }
    return;
  }
  refresh_voice_peer_runtime_state(st);
}

}  // namespace agentd
