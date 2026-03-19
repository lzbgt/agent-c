#include "edge_consensus_runtime_lifecycle.h"

#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <cstring>
#endif

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32)
static bool pid_is_running(pid_t pid) {
  if (pid <= 0) return false;
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM;
}
#endif

}  // namespace

void refresh_edge_consensus_runtime_state(EdgeConsensusRuntime* st) {
  if (!st) return;
#if !defined(_WIN32)
  if (st->runtime_kind == "external" && st->running && !pid_is_running(st->pid)) {
    st->running = false;
    if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  }
#endif
}

void finalize_recovered_edge_consensus_stop(EdgeConsensusRuntime* st, int signal_used) {
  if (!st || signal_used <= 0) return;
  if (st->status_source != "persisted" || st->running) return;
  if (st->exit_signal != 0) return;
  if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  st->exit_signal = signal_used;
}

bool edge_consensus_runtime_kill_best_effort(
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  int* out_signal_used,
  std::string* out_err
) {
  if (out_signal_used) *out_signal_used = 0;
  if (out_err) out_err->clear();
  if (!st) return false;
  refresh_edge_consensus_runtime_state(st.get());
  if (!st->running) return true;
  if (st->runtime_kind == "builtin") {
    if (st->stop_requested) st->stop_requested->store(true);
    const int64_t deadline = now_unix_ms() + 4000;
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(runtime_mu);
        if (!st->running) return true;
      }
      if (now_unix_ms() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (out_err) *out_err = "builtin runtime stop timeout";
    return false;
  }
#if defined(_WIN32)
  if (out_err) *out_err = "external consensus runtime stop unsupported on Windows";
  return false;
#else
  if (::kill(st->pid, SIGTERM) != 0) {
    if (errno == ESRCH) return true;
    if (out_err) *out_err = std::string("kill(SIGTERM) failed: ") + std::strerror(errno);
    return false;
  }
  if (out_signal_used) *out_signal_used = SIGTERM;
  const int64_t deadline = now_unix_ms() + 1500;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      refresh_edge_consensus_runtime_state(st.get());
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (::kill(st->pid, SIGKILL) != 0 && errno != ESRCH) {
    if (out_err) *out_err = std::string("kill(SIGKILL) failed: ") + std::strerror(errno);
    return false;
  }
  if (out_signal_used) *out_signal_used = SIGKILL;
  return true;
#endif
}

bool edge_consensus_runtime_confirm_startup(
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  Json::Value* out_runtime,
  std::string* out_err,
  int64_t timeout_ms
) {
  if (out_err) out_err->clear();
  if (out_runtime) *out_runtime = Json::Value(Json::nullValue);
  if (!st) {
    if (out_err) *out_err = "missing runtime";
    return false;
  }

  const int64_t deadline = now_unix_ms() + timeout_ms;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      if (st->startup_ready && st->startup_ready->load()) {
        if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
        return true;
      }
      if (!st->running) {
        const bool completed_ok =
          st->exit_code == 0 ||
          (st->last_stdout_json.isObject() &&
           st->last_stdout_json.isMember("ok") &&
           st->last_stdout_json["ok"].isBool() &&
           st->last_stdout_json["ok"].asBool());
        if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
        if (completed_ok) return true;
        if (out_err) {
          *out_err = !st->last_error.empty() ? st->last_error : "consensus runtime exited before startup confirmation";
        }
        return false;
      }
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    if (st->startup_ready && !st->startup_ready->load()) {
      if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
      if (out_err) *out_err = "consensus runtime did not confirm startup";
      return false;
    }
  }
  if (out_runtime) {
    std::lock_guard<std::mutex> lk(runtime_mu);
    *out_runtime = edge_consensus_runtime_to_json(*st);
  }
  return true;
}

}  // namespace agentd
