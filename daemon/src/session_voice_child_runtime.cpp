#include "session_voice_child_runtime.h"

#include "json_util.h"
#include "session_voice_runtime_seed.h"
#include "string_util.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <csignal>
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

static bool read_last_nonempty_line(const std::string& path, std::string* out_line) {
  if (out_line) *out_line = "";
  if (trim_copy(path).empty()) return false;
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::string line;
  std::string last;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (!line.empty()) last = line;
  }
  if (last.empty()) return false;
  if (out_line) *out_line = last;
  return true;
}

static std::filesystem::path voice_peer_runtime_dir(const DaemonConfig& cfg, const std::string& session_id) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "voice_webrtc_peers" / session_id;
}

#if !defined(_WIN32)
static bool pid_is_running(pid_t pid) {
  if (pid <= 0) return false;
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM;
}
#else
static bool pid_is_running(intptr_t pid) {
  (void)pid;
  return false;
}
#endif

}  // namespace

void refresh_voice_peer_runtime_state(VoicePeerRuntime* st) {
  if (!st) return;
  if (!st->ready && !st->ready_file_path.empty() && std::filesystem::exists(st->ready_file_path)) st->ready = true;
  if (!st->stdout_log_path.empty()) {
    std::string last_line;
    if (read_last_nonempty_line(st->stdout_log_path, &last_line)) {
      st->last_stdout_line = last_line;
      Json::Value parsed(Json::nullValue);
      std::string jerr;
      if (json_parse_any(last_line, &parsed, &jerr) && parsed.isObject()) {
        st->last_stdout_json = parsed;
        if (parsed.isMember("error") && parsed["error"].isString()) st->last_error = parsed["error"].asString();
      }
    }
  }
  if (st->running && !pid_is_running(st->pid)) {
    st->running = false;
    if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  }
}

void finalize_recovered_voice_peer_stop(VoicePeerRuntime* st, int signal_used) {
  if (!st) return;
  if (st->status_source != "persisted" || st->running) return;
  if (st->last_stdout_json.isObject() && st->last_stdout_json.isMember("reason") && st->last_stdout_json["reason"].isString()) {
    const std::string reason = lower_copy(trim_copy(st->last_stdout_json["reason"].asString()));
    if (reason == "sigterm") signal_used = SIGTERM;
    else if (reason == "sigint") signal_used = SIGINT;
  }
  if (signal_used <= 0 || st->exit_signal != 0) return;
  if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  st->exit_signal = signal_used;
}

bool wait_for_voice_peer_stop(const std::shared_ptr<VoicePeerRuntime>& st, std::mutex& runtime_mu, int64_t timeout_ms) {
  if (!st) return true;
  const int64_t deadline = now_unix_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      refresh_voice_peer_runtime_state(st.get());
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_state(st.get());
    return !st->running;
  }
}

VoicePeerStartupWaitResult wait_for_voice_peer_startup(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms
) {
  VoicePeerStartupWaitResult result;
  if (!st) return result;
  const int64_t deadline = now_unix_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      refresh_voice_peer_runtime_state(st.get());
      result.ready = st->ready;
      result.running = st->running;
      if (result.ready || !result.running) return result;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_voice_peer_runtime_state(st.get());
    result.ready = st->ready;
    result.running = st->running;
    result.timed_out = !result.ready && result.running;
  }
  return result;
}

bool voice_peer_spawn_process(
  const DaemonConfig& cfg,
  const VoicePeerChildLaunchConfig& launch_cfg,
  std::mutex& runtime_mu,
  const std::function<void(const VoicePeerRuntime&)>& on_exit_persist,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  if (launch_cfg.tool_path.empty()) {
    if (out_err) *out_err = "audio_webrtc_peer tool path not configured";
    return false;
  }
  if (!std::filesystem::exists(std::filesystem::path(launch_cfg.tool_path))) {
    if (out_err) *out_err = "audio_webrtc_peer tool path not found";
    return false;
  }

  std::error_code ec;
  const std::filesystem::path run_dir = voice_peer_runtime_dir(cfg, launch_cfg.session_id);
  std::filesystem::create_directories(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create voice peer runtime dir";
    return false;
  }
  const std::filesystem::path ready_file = run_dir / "ready.json";
  const std::filesystem::path stdout_log = run_dir / "stdout.jsonl";
  const std::filesystem::path stderr_log = run_dir / "stderr.log";
  std::filesystem::remove(ready_file, ec);
  ec.clear();
  std::filesystem::remove(stdout_log, ec);
  ec.clear();

#if defined(_WIN32)
  (void)runtime_mu;
  (void)on_exit_persist;
  (void)ready_file;
  (void)stdout_log;
  (void)stderr_log;
  if (out_err) *out_err = "voice_webrtc_peer start unsupported on Windows";
  return false;
#else
  const int stdout_fd = ::open(stdout_log.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (stdout_fd < 0) {
    if (out_err) *out_err = std::string("open stdout log failed: ") + std::strerror(errno);
    return false;
  }
  const int stderr_fd = ::open(stderr_log.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (stderr_fd < 0) {
    if (out_err) *out_err = std::string("open stderr log failed: ") + std::strerror(errno);
    close(stdout_fd);
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    if (out_err) *out_err = std::string("fork failed: ") + std::strerror(errno);
    close(stdout_fd);
    close(stderr_fd);
    return false;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    (void)dup2(stdout_fd, STDOUT_FILENO);
    (void)dup2(stderr_fd, STDERR_FILENO);
    close(stdout_fd);
    close(stderr_fd);
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 4096;
    for (int fd = 3; fd < max_fd; ++fd) close(fd);

    const std::string deadline_s = std::to_string((long long)launch_cfg.deadline_ms);
    const std::string poll_s = std::to_string((long long)launch_cfg.poll_interval_ms);
    const std::string tone_s = std::to_string((long long)launch_cfg.tone_hz);

    std::vector<std::string> args;
    args.push_back(launch_cfg.node_bin);
    args.push_back(launch_cfg.tool_path);
    args.push_back("--broker-url");
    args.push_back(launch_cfg.broker_url);
    args.push_back("--token");
    args.push_back(launch_cfg.broker_token);
    args.push_back("--session-id");
    args.push_back(launch_cfg.broker_session_id);
    args.push_back("--ready-file");
    args.push_back(ready_file.string());
    args.push_back("--deadline-ms");
    args.push_back(deadline_s);
    args.push_back("--poll-interval-ms");
    args.push_back(poll_s);
    args.push_back("--tone-hz");
    args.push_back(tone_s);
    if (!launch_cfg.sender_tag.empty()) {
      args.push_back("--sender-tag");
      args.push_back(launch_cfg.sender_tag);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(launch_cfg.node_bin.c_str(), argv.data());
    _exit(127);
  }

  close(stdout_fd);
  close(stderr_fd);

  VoicePeerRuntimeSeed runtime_seed;
  runtime_seed.runtime_kind = launch_cfg.runtime_kind;
  runtime_seed.session_id = launch_cfg.session_id;
  runtime_seed.broker_session_id = launch_cfg.broker_session_id;
  runtime_seed.broker_url = launch_cfg.broker_url;
  runtime_seed.broker_agent_id = launch_cfg.broker_agent_id;
  runtime_seed.broker_deployment_id = launch_cfg.broker_deployment_id;
  runtime_seed.sender_tag = launch_cfg.sender_tag;
  runtime_seed.tool_path = launch_cfg.tool_path;
  runtime_seed.node_bin = launch_cfg.node_bin;
  runtime_seed.ready_file_path = ready_file.string();
  runtime_seed.stdout_log_path = stdout_log.string();
  runtime_seed.stderr_log_path = stderr_log.string();
  runtime_seed.deadline_ms = launch_cfg.deadline_ms;
  runtime_seed.poll_interval_ms = launch_cfg.poll_interval_ms;
  runtime_seed.tone_hz = launch_cfg.tone_hz;
  runtime_seed.managed_broker_session = launch_cfg.managed_broker_session;
  runtime_seed.ready = false;
  runtime_seed.running = true;
  runtime_seed.pid = pid;
  auto st = make_voice_peer_runtime_state(runtime_seed);

  std::mutex* runtime_mu_ptr = &runtime_mu;
  std::thread([st, runtime_mu_ptr, on_exit_persist]() {
    int status = 0;
    (void)waitpid(st->pid, &status, 0);
    bool should_persist = false;
    VoicePeerRuntime persisted_snapshot;
    {
      std::lock_guard<std::mutex> lk(*runtime_mu_ptr);
      refresh_voice_peer_runtime_state(st.get());
      st->running = false;
      st->ready = std::filesystem::exists(st->ready_file_path);
      st->ended_unix_ms = now_unix_ms();
      if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
      should_persist = !st->suppress_persist;
      if (should_persist) persisted_snapshot = *st;
    }
    if (should_persist && on_exit_persist) on_exit_persist(persisted_snapshot);
  }).detach();

  *out_state = std::move(st);
  return true;
#endif
}

bool voice_peer_kill_best_effort(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int* out_signal_used,
  std::string* out_err
) {
  if (out_signal_used) *out_signal_used = 0;
  if (out_err) out_err->clear();
  if (!st) return false;
#if defined(_WIN32)
  (void)runtime_mu;
  if (st->running) {
    if (out_err) *out_err = "voice_webrtc_peer stop unsupported on Windows";
    return false;
  }
  return true;
#else
  if (!st->running) return true;
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

bool remove_voice_peer_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id,
  bool* out_any_deleted,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_any_deleted) *out_any_deleted = false;
  if (trim_copy(session_id).empty()) return true;
  std::error_code ec;
  const std::filesystem::path run_dir = voice_peer_runtime_dir(cfg, session_id);
  if (!std::filesystem::exists(run_dir, ec)) return true;
  ec.clear();
  const auto removed = std::filesystem::remove_all(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to remove voice peer runtime artifacts";
    return false;
  }
  if (out_any_deleted) *out_any_deleted = (removed > 0);
  return true;
}

}  // namespace agentd
