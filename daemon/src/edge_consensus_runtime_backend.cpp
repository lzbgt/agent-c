#include "edge_consensus_runtime_backend.h"

#include "agent_db.h"
#include "edge_consensus_runtime_policy.h"
#include "json_util.h"
#include "string_util.h"

#include <chrono>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::shared_ptr<EdgeConsensusRuntime> make_edge_consensus_external_runtime_state(
  const EdgeConsensusRuntime& runtime_state,
  const EdgeConsensusExternalProcessPlan& process_plan,
#if defined(_WIN32)
  intptr_t pid
#else
  pid_t pid
#endif
) {
  auto st = std::make_shared<EdgeConsensusRuntime>(runtime_state);
  st->runtime_kind = "external";
  st->tool_path = process_plan.tool_path;
  st->stderr_log_path = process_plan.artifacts.stderr_log_path;
  st->pid = pid;
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);
  return st;
}

std::shared_ptr<EdgeConsensusRuntime> make_edge_consensus_builtin_runtime_state(
  const EdgeConsensusRuntime& runtime_state
) {
  auto st = std::make_shared<EdgeConsensusRuntime>(runtime_state);
  st->runtime_kind = "builtin";
  st->tool_path = "@builtin";
  st->daemon_url = "@local";
  st->stop_requested = std::make_shared<std::atomic<bool>>(false);
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);
  return st;
}

void apply_edge_consensus_runtime_terminal_result(
  EdgeConsensusRuntime* st,
  bool ok,
  const Json::Value& result,
  const std::string& err
) {
  if (!st) return;
  st->running = false;
  st->ended_unix_ms = now_unix_ms();
  st->live_status_json = Json::Value(Json::nullValue);
  st->last_stdout_json = result;
  if (!result.isNull()) st->last_stdout_line = json_stringify(result);
  if (!ok) {
    st->exit_code = 1;
    st->last_error = err;
    return;
  }
  st->exit_code = result.isObject() && result.isMember("ok") && result["ok"].asBool() ? 0 : 1;
  if (result.isObject() && result.isMember("error") && result["error"].isString()) {
    st->last_error = result["error"].asString();
  }
}

#if !defined(_WIN32)
bool edge_consensus_runtime_spawn_external(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  std::mutex& runtime_mu,
  const EdgeConsensusRuntimePersistFn& on_exit_persist,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  EdgeConsensusHttpRuntimeConfig run_cfg;
  EdgeConsensusRuntime runtime_state;
  if (!edge_consensus_runtime_build_config(cfg, body, &run_cfg, &runtime_state, out_err)) return false;

  const std::string unavailable_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  if (!unavailable_reason.empty()) {
    if (out_err) *out_err = unavailable_reason;
    return false;
  }

  const EdgeConsensusExternalProcessPlan process_plan =
    make_edge_consensus_external_process_plan(cfg, run_cfg, runtime_state);

  std::error_code ec;
  const std::filesystem::path run_dir = process_plan.artifacts.runtime_dir;
  std::filesystem::create_directories(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create edge consensus runtime dir";
    return false;
  }
  const std::filesystem::path stderr_log = process_plan.artifacts.stderr_log_path;

  int out_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0) {
    if (out_err) *out_err = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }
  const int stderr_fd = ::open(stderr_log.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (stderr_fd < 0) {
    if (out_err) *out_err = std::string("open stderr log failed: ") + std::strerror(errno);
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    if (out_err) *out_err = std::string("fork failed: ") + std::strerror(errno);
    close(stderr_fd);
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    (void)dup2(stderr_fd, STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(stderr_fd);
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 4096;
    for (int fd = 3; fd < max_fd; ++fd) close(fd);

    std::vector<char*> argv;
    argv.reserve(process_plan.argv.size() + 1);
    for (const auto& arg : process_plan.argv) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(process_plan.tool_path.c_str(), argv.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(stderr_fd);

  auto st = make_edge_consensus_external_runtime_state(runtime_state, process_plan, pid);

  std::thread([st, &runtime_mu, on_exit_persist, fd = out_pipe[0]]() {
    std::string buffer;
    char chunk[4096];
    for (;;) {
      const ssize_t n = ::read(fd, chunk, sizeof(chunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        std::lock_guard<std::mutex> lk(runtime_mu);
        st->last_error = std::string("read failed: ") + std::strerror(errno);
        break;
      }
      if (n == 0) break;
      buffer.append(chunk, (size_t)n);
      for (;;) {
        const size_t nl = buffer.find('\n');
        if (nl == std::string::npos) break;
        std::string line = trim_copy(buffer.substr(0, nl));
        buffer.erase(0, nl + 1);
        if (line.empty()) continue;
        Json::Value parsed(Json::nullValue);
        std::string jerr;
        if (json_parse_any(line, &parsed, &jerr) && parsed.isObject()) {
          Json::Value live_status(Json::nullValue);
          std::lock_guard<std::mutex> lk(runtime_mu);
          if (edge_consensus_runtime_stdout_event_is_startup_ready(parsed)) {
            if (st->startup_ready) st->startup_ready->store(true);
            continue;
          }
          if (edge_consensus_runtime_stdout_event_live_status(parsed, &live_status)) {
            st->live_status_json = live_status;
            continue;
          }
          st->last_stdout_line = line;
          st->last_stdout_json = parsed;
          if (parsed.isMember("error") && parsed["error"].isString()) st->last_error = parsed["error"].asString();
        } else {
          std::lock_guard<std::mutex> lk(runtime_mu);
          st->last_stdout_line = line;
        }
      }
    }
    close(fd);
    int status = 0;
    (void)waitpid(st->pid, &status, 0);
    EdgeConsensusRuntime persisted_snapshot;
    bool should_persist = false;
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      st->running = false;
      st->ended_unix_ms = now_unix_ms();
      st->live_status_json = Json::Value(Json::nullValue);
      if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
      persisted_snapshot = *st;
      should_persist = true;
    }
    if (should_persist && on_exit_persist) on_exit_persist(persisted_snapshot);
  }).detach();

  *out_state = std::move(st);
  (void)db;
  return true;
}

bool edge_consensus_runtime_start_builtin(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  std::mutex& runtime_mu,
  const EdgeConsensusRuntimePersistFn& on_exit_persist,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  EdgeConsensusHttpRuntimeConfig run_cfg;
  EdgeConsensusRuntime runtime_state;
  if (!edge_consensus_runtime_build_config(cfg, body, &run_cfg, &runtime_state, out_err)) return false;

  auto st = make_edge_consensus_builtin_runtime_state(runtime_state);

  std::thread([db, st, run_cfg, &runtime_mu, on_exit_persist]() mutable {
    EdgeConsensusHttpRuntimeHooks hooks;
    hooks.stop_requested = st->stop_requested.get();
    hooks.log_line = [st, &runtime_mu](const std::string& line) {
      std::lock_guard<std::mutex> lk(runtime_mu);
      st->last_stdout_line = line;
    };
    hooks.status_update = [st, &runtime_mu](const Json::Value& status) {
      std::lock_guard<std::mutex> lk(runtime_mu);
      st->live_status_json = status;
    };
    hooks.startup_ready = [st]() {
      if (st->startup_ready) st->startup_ready->store(true);
    };
    Json::Value result(Json::nullValue);
    std::string err;
    const bool ok = run_edge_consensus_local_runtime(db, run_cfg, hooks, &result, &err);
    EdgeConsensusRuntime persisted_snapshot;
    bool should_persist = false;
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      apply_edge_consensus_runtime_terminal_result(st.get(), ok, result, err);
      persisted_snapshot = *st;
      should_persist = true;
    }
    if (should_persist && on_exit_persist) on_exit_persist(persisted_snapshot);
  }).detach();

  *out_state = std::move(st);
  (void)cfg;
  return true;
}
#endif

}  // namespace agentd
