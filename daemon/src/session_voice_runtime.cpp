#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
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

static bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static bool is_safe_shellish_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    if (!ok) return false;
  }
  return true;
}

struct VoicePeerRuntime {
  std::string runtime_kind = "external";
  std::string status_source = "memory";
  std::string session_id;
  std::string broker_session_id;
  std::string broker_url;
  std::string sender_tag;
  std::string tool_path;
  std::string node_bin;
  std::string ready_file_path;
  std::string stdout_log_path;
  std::string stderr_log_path;
  int64_t started_unix_ms = 0;
  int64_t ended_unix_ms = 0;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  bool ready = false;
  bool running = false;
  int exit_code = 0;
  int exit_signal = 0;
  std::string last_error;
  std::string last_stdout_line;
  Json::Value last_stdout_json = Json::Value(Json::nullValue);
#if defined(_WIN32)
  intptr_t pid = 0;
#else
  pid_t pid = -1;
#endif
};

static std::mutex g_voice_peer_mu;
static std::unordered_map<std::string, std::shared_ptr<VoicePeerRuntime>> g_voice_peer_by_session;

static Json::Value voice_peer_runtime_to_json(const VoicePeerRuntime& st) {
  Json::Value out(Json::objectValue);
  out["schema"] = "session_voice_webrtc_peer_runtime_v1";
  out["runtime_kind"] = st.runtime_kind;
  out["status_source"] = st.status_source.empty() ? "memory" : st.status_source;
  out["session_id"] = st.session_id;
  if (!st.broker_session_id.empty()) out["broker_session_id"] = st.broker_session_id;
  out["broker_url"] = st.broker_url;
  out["sender_tag"] = st.sender_tag;
  out["tool_path"] = st.tool_path;
  out["node_bin"] = st.node_bin;
  if (!st.stdout_log_path.empty()) out["stdout_log_path"] = st.stdout_log_path;
  out["started_unix_ms"] = (Json::Int64)st.started_unix_ms;
  if (st.ended_unix_ms > 0) out["ended_unix_ms"] = (Json::Int64)st.ended_unix_ms;
  out["deadline_ms"] = (Json::Int64)st.deadline_ms;
  out["poll_interval_ms"] = (Json::Int64)st.poll_interval_ms;
  out["tone_hz"] = (Json::Int64)st.tone_hz;
  out["ready"] = st.ready;
  out["running"] = st.running;
  if (!st.stderr_log_path.empty()) out["stderr_log_path"] = st.stderr_log_path;
  if (!st.ready_file_path.empty()) out["ready_file_path"] = st.ready_file_path;
  if (st.running) {
    out["pid"] = (Json::Int64)st.pid;
  } else {
    out["exit_code"] = st.exit_code;
    if (st.exit_signal != 0) out["exit_signal"] = st.exit_signal;
  }
  if (!st.last_error.empty()) out["last_error"] = st.last_error;
  if (!st.last_stdout_line.empty()) out["last_stdout_line"] = st.last_stdout_line;
  if (!st.last_stdout_json.isNull()) out["last_stdout"] = st.last_stdout_json;
  return out;
}

static std::shared_ptr<VoicePeerRuntime> voice_peer_lookup_locked(const std::string& session_id) {
  const auto it = g_voice_peer_by_session.find(session_id);
  return it == g_voice_peer_by_session.end() ? nullptr : it->second;
}

static std::string voice_peer_meta_key(const std::string& session_id) {
  return "session.voice_webrtc_peer." + session_id;
}

static std::filesystem::path voice_peer_runtime_dir(const DaemonConfig& cfg, const std::string& session_id) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "voice_webrtc_peers" / session_id;
}

static bool read_last_nonempty_line(const std::string& path, std::string* out_line) {
  if (out_line) out_line->clear();
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

static std::string bundled_audio_peer_tool_name() {
  return "agentd_audio_webrtc_peer.js";
}

static std::string normalized_path_string(const std::filesystem::path& p) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(p, ec);
  return (ec ? p : abs).lexically_normal().string();
}

static std::string discover_bundled_audio_peer_tool_path(const DaemonConfig& cfg) {
  std::vector<std::filesystem::path> candidates;
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec && !cwd.empty()) {
    candidates.push_back(cwd / "tools" / bundled_audio_peer_tool_name());
    candidates.push_back(cwd.parent_path() / "tools" / bundled_audio_peer_tool_name());
  }
  if (!cfg.state_dir.empty()) {
    const std::filesystem::path state_root = std::filesystem::path(cfg.state_dir).lexically_normal();
    candidates.push_back(state_root / "tools" / bundled_audio_peer_tool_name());
    candidates.push_back(state_root.parent_path() / "tools" / bundled_audio_peer_tool_name());
    candidates.push_back(state_root.parent_path().parent_path() / "tools" / bundled_audio_peer_tool_name());
  }

  std::set<std::string> seen;
  for (const auto& candidate : candidates) {
    const std::string normalized = normalized_path_string(candidate);
    if (!seen.insert(normalized).second) continue;
    std::error_code fsec;
    if (std::filesystem::exists(candidate, fsec) && std::filesystem::is_regular_file(candidate, fsec)) {
      return normalized;
    }
  }
  return "";
}

static std::string default_voice_peer_runtime_kind(const DaemonConfig& cfg) {
  return discover_bundled_audio_peer_tool_path(cfg).empty() ? "external" : "bundled";
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

static void refresh_voice_peer_runtime_state(VoicePeerRuntime* st) {
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

static bool voice_peer_runtime_from_json(const Json::Value& v, VoicePeerRuntime* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  if (!v.isObject()) {
    if (out_err) *out_err = "runtime record must be an object";
    return false;
  }
  VoicePeerRuntime st;
  if (v.isMember("runtime_kind") && v["runtime_kind"].isString()) st.runtime_kind = trim_copy(v["runtime_kind"].asString());
  if (v.isMember("status_source") && v["status_source"].isString()) st.status_source = trim_copy(v["status_source"].asString());
  if (v.isMember("session_id") && v["session_id"].isString()) st.session_id = trim_copy(v["session_id"].asString());
  if (v.isMember("broker_session_id") && v["broker_session_id"].isString()) st.broker_session_id = trim_copy(v["broker_session_id"].asString());
  if (v.isMember("broker_url") && v["broker_url"].isString()) st.broker_url = v["broker_url"].asString();
  if (v.isMember("sender_tag") && v["sender_tag"].isString()) st.sender_tag = trim_copy(v["sender_tag"].asString());
  if (v.isMember("tool_path") && v["tool_path"].isString()) st.tool_path = v["tool_path"].asString();
  if (v.isMember("node_bin") && v["node_bin"].isString()) st.node_bin = trim_copy(v["node_bin"].asString());
  if (v.isMember("ready_file_path") && v["ready_file_path"].isString()) st.ready_file_path = v["ready_file_path"].asString();
  if (v.isMember("stdout_log_path") && v["stdout_log_path"].isString()) st.stdout_log_path = v["stdout_log_path"].asString();
  if (v.isMember("stderr_log_path") && v["stderr_log_path"].isString()) st.stderr_log_path = v["stderr_log_path"].asString();
  if (v.isMember("started_unix_ms") && (v["started_unix_ms"].isInt64() || v["started_unix_ms"].isUInt64())) st.started_unix_ms = v["started_unix_ms"].asInt64();
  if (v.isMember("ended_unix_ms") && (v["ended_unix_ms"].isInt64() || v["ended_unix_ms"].isUInt64())) st.ended_unix_ms = v["ended_unix_ms"].asInt64();
  if (v.isMember("deadline_ms") && (v["deadline_ms"].isInt64() || v["deadline_ms"].isUInt64())) st.deadline_ms = v["deadline_ms"].asInt64();
  if (v.isMember("poll_interval_ms") && (v["poll_interval_ms"].isInt64() || v["poll_interval_ms"].isUInt64())) st.poll_interval_ms = v["poll_interval_ms"].asInt64();
  if (v.isMember("tone_hz") && (v["tone_hz"].isInt64() || v["tone_hz"].isUInt64())) st.tone_hz = v["tone_hz"].asInt64();
  if (v.isMember("ready") && v["ready"].isBool()) st.ready = v["ready"].asBool();
  if (v.isMember("running") && v["running"].isBool()) st.running = v["running"].asBool();
  if (v.isMember("exit_code") && v["exit_code"].isInt()) st.exit_code = v["exit_code"].asInt();
  if (v.isMember("exit_signal") && v["exit_signal"].isInt()) st.exit_signal = v["exit_signal"].asInt();
  if (v.isMember("last_error") && v["last_error"].isString()) st.last_error = v["last_error"].asString();
  if (v.isMember("last_stdout_line") && v["last_stdout_line"].isString()) st.last_stdout_line = v["last_stdout_line"].asString();
  if (v.isMember("last_stdout") && v["last_stdout"].isObject()) st.last_stdout_json = v["last_stdout"];
  if (v.isMember("pid") && (v["pid"].isInt64() || v["pid"].isUInt64())) st.pid = (decltype(st.pid))v["pid"].asInt64();
  *out = std::move(st);
  return true;
}

static bool persist_voice_peer_runtime_record(AgentDb* db, const VoicePeerRuntime& st, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  Json::Value record = voice_peer_runtime_to_json(st);
  record["persisted_utc_ms"] = (Json::Int64)now_unix_ms();
  return db->meta_set(voice_peer_meta_key(st.session_id), json_stringify(record), out_err);
}

static bool load_voice_peer_runtime_record(
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  std::string raw;
  if (!db->meta_get(voice_peer_meta_key(session_id), &raw, out_err)) return false;
  if (trim_copy(raw).empty()) return true;
  Json::Value parsed(Json::nullValue);
  std::string jerr;
  if (!json_parse_any(raw, &parsed, &jerr) || !parsed.isObject()) {
    if (out_err) *out_err = jerr.empty() ? "persisted voice runtime record corrupt" : jerr;
    return false;
  }
  auto st = std::make_shared<VoicePeerRuntime>();
  if (!voice_peer_runtime_from_json(parsed, st.get(), out_err)) return false;
  st->status_source = "persisted";
  refresh_voice_peer_runtime_state(st.get());
  if (out_state) *out_state = std::move(st);
  return true;
}

static void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out) {
  if (!out) return;
  const std::string bundled_tool_path = discover_bundled_audio_peer_tool_path(cfg);
  (*out)["builtin_available"] = false;
  (*out)["bundled_available"] = !bundled_tool_path.empty();
  (*out)["default_runtime_kind"] = default_voice_peer_runtime_kind(cfg);
  (*out)["tool_configured"] = !trim_copy(cfg.audio_webrtc_peer_tool_path).empty();
  if (!cfg.audio_webrtc_peer_tool_path.empty()) (*out)["tool_path"] = cfg.audio_webrtc_peer_tool_path;
  if (!bundled_tool_path.empty()) (*out)["bundled_tool_path"] = bundled_tool_path;
  (*out)["node_bin"] = cfg.audio_webrtc_peer_node_bin.empty() ? "node" : cfg.audio_webrtc_peer_node_bin;
}

static bool resolve_voice_peer_backend(
  const DaemonConfig& cfg,
  const std::string& runtime_kind,
  std::string* out_tool_path,
  std::string* out_node_bin,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_tool_path) out_tool_path->clear();
  const std::string kind = lower_copy(trim_copy(runtime_kind));
  const std::string node_bin = trim_copy(cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);
  if (!is_safe_shellish_token(node_bin, 256)) {
    if (out_err) *out_err = "invalid audio_webrtc_peer_node_bin";
    return false;
  }
  if (out_node_bin) *out_node_bin = node_bin;
  if (kind == "bundled") {
    const std::string bundled = discover_bundled_audio_peer_tool_path(cfg);
    if (bundled.empty()) {
      if (out_err) *out_err = "bundled voice_webrtc_peer tool unavailable";
      return false;
    }
    if (out_tool_path) *out_tool_path = bundled;
    return true;
  }
  if (kind == "external") {
    const std::string tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
    if (tool_path.empty()) {
      if (out_err) *out_err = "audio_webrtc_peer_tool_path not configured";
      return false;
    }
    if (out_tool_path) *out_tool_path = tool_path;
    return true;
  }
  if (out_err) *out_err = "runtime_kind must be bundled, external, or builtin";
  return false;
}

#if !defined(_WIN32)
static bool voice_peer_spawn_process(
  const DaemonConfig& cfg,
  const std::string& runtime_kind,
  const std::string& tool_path,
  const std::string& node_bin,
  const std::string& session_id,
  const std::string& broker_session_id,
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& sender_tag,
  int64_t deadline_ms,
  int64_t poll_interval_ms,
  int64_t tone_hz,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  if (tool_path.empty()) {
    if (out_err) *out_err = "audio_webrtc_peer tool path not configured";
    return false;
  }
  if (!std::filesystem::exists(std::filesystem::path(tool_path))) {
    if (out_err) *out_err = "audio_webrtc_peer tool path not found";
    return false;
  }

  std::error_code ec;
  const std::filesystem::path run_dir = voice_peer_runtime_dir(cfg, session_id);
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

    const std::string deadline_s = std::to_string((long long)deadline_ms);
    const std::string poll_s = std::to_string((long long)poll_interval_ms);
    const std::string tone_s = std::to_string((long long)tone_hz);

    std::vector<std::string> args;
    args.push_back(node_bin);
    args.push_back(tool_path);
    args.push_back("--broker-url");
    args.push_back(broker_url);
    args.push_back("--token");
    args.push_back(broker_token);
    args.push_back("--session-id");
    args.push_back(broker_session_id);
    args.push_back("--ready-file");
    args.push_back(ready_file.string());
    args.push_back("--deadline-ms");
    args.push_back(deadline_s);
    args.push_back("--poll-interval-ms");
    args.push_back(poll_s);
    args.push_back("--tone-hz");
    args.push_back(tone_s);
    if (!sender_tag.empty()) {
      args.push_back("--sender-tag");
      args.push_back(sender_tag);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(node_bin.c_str(), argv.data());
    _exit(127);
  }

  close(stdout_fd);
  close(stderr_fd);

  auto st = std::make_shared<VoicePeerRuntime>();
  st->runtime_kind = runtime_kind;
  st->session_id = session_id;
  st->broker_session_id = broker_session_id;
  st->broker_url = broker_url;
  st->sender_tag = sender_tag;
  st->tool_path = tool_path;
  st->node_bin = node_bin;
  st->ready_file_path = ready_file.string();
  st->stdout_log_path = stdout_log.string();
  st->stderr_log_path = stderr_log.string();
  st->started_unix_ms = now_unix_ms();
  st->deadline_ms = deadline_ms;
  st->poll_interval_ms = poll_interval_ms;
  st->tone_hz = tone_hz;
  st->ready = false;
  st->running = true;
  st->pid = pid;

  std::thread([st]() {
    int status = 0;
    (void)waitpid(st->pid, &status, 0);
    refresh_voice_peer_runtime_state(st.get());
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    st->running = false;
    st->ready = std::filesystem::exists(st->ready_file_path);
    st->ended_unix_ms = now_unix_ms();
    if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
  }).detach();

  *out_state = std::move(st);
  return true;
}

static bool voice_peer_kill_best_effort(std::shared_ptr<VoicePeerRuntime> st, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!st) return false;
  if (!st->running) return true;
  if (::kill(st->pid, SIGTERM) != 0) {
    if (errno == ESRCH) return true;
    if (out_err) *out_err = std::string("kill(SIGTERM) failed: ") + std::strerror(errno);
    return false;
  }
  const int64_t deadline = now_unix_ms() + 1500;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (::kill(st->pid, SIGKILL) != 0 && errno != ESRCH) {
    if (out_err) *out_err = std::string("kill(SIGKILL) failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}
#endif

}  // namespace

void handle_session_voice_webrtc_peer_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }

  const std::string session_id = body.isMember("session_id") && body["session_id"].isString() ? trim_copy(body["session_id"].asString()) : "";
  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  bool session_exists = false;
  std::string err;
  if (!db->session_exists(session_id, &session_exists, &err)) {
    resp->status = 500;
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to query session" : err;
    voice_peer_add_runtime_metadata(cfg, &out);
    resp->body = json_stringify(out);
    return;
  }
  if (!session_exists) {
    resp->status = 404;
    resp->body = json_error_body("session not found");
    return;
  }

  const std::string action = body.isMember("action") && body["action"].isString() ? lower_copy(trim_copy(body["action"].asString())) : "";
  if (action != "start" && action != "stop") {
    resp->status = 400;
    resp->body = json_error_body("action must be start or stop");
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["session_id"] = session_id;
  voice_peer_add_runtime_metadata(cfg, &out);

  const std::string runtime_kind = body.isMember("runtime_kind") && body["runtime_kind"].isString()
    ? lower_copy(trim_copy(body["runtime_kind"].asString()))
    : default_voice_peer_runtime_kind(cfg);
  if (runtime_kind != "external" && runtime_kind != "bundled" && runtime_kind != "builtin") {
    resp->status = 400;
    resp->body = json_error_body("runtime_kind must be bundled, external, or builtin");
    return;
  }
  if (runtime_kind == "builtin") {
    out["error"] = "builtin voice_webrtc_peer runtime not implemented";
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
  }

  if (action == "stop") {
    std::shared_ptr<VoicePeerRuntime> st;
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      st = voice_peer_lookup_locked(session_id);
      if (st) refresh_voice_peer_runtime_state(st.get());
    }
    if (!st) {
      std::string lerr;
      if (!load_voice_peer_runtime_record(db, session_id, &st, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
    }
    if (!st) {
      out["ok"] = true;
      out["stopped"] = false;
      out["reason"] = "not_running";
      out["peer"] = Json::Value(Json::nullValue);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
#if defined(_WIN32)
    out["error"] = "voice_webrtc_peer stop unsupported on Windows";
    out["peer"] = voice_peer_runtime_to_json(*st);
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
#else
    std::string serr;
    if (!voice_peer_kill_best_effort(st, &serr)) {
      out["error"] = serr.empty() ? "failed to stop voice peer" : serr;
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        refresh_voice_peer_runtime_state(st.get());
        out["peer"] = voice_peer_runtime_to_json(*st);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      out["peer"] = voice_peer_runtime_to_json(*st);
    }
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    out["ok"] = true;
    out["stopped"] = true;
    resp->status = 200;
    resp->body = json_stringify(out);
    return;
#endif
  }

  int64_t deadline_ms = 15000;
  if (body.isMember("deadline_ms") && (body["deadline_ms"].isInt64() || body["deadline_ms"].isUInt64() || body["deadline_ms"].isInt())) {
    deadline_ms = body["deadline_ms"].asInt64();
  }
  if (deadline_ms < 1000) deadline_ms = 1000;
  if (deadline_ms > 120000) deadline_ms = 120000;

  int64_t poll_interval_ms = 100;
  if (body.isMember("poll_interval_ms") && (body["poll_interval_ms"].isInt64() || body["poll_interval_ms"].isUInt64() || body["poll_interval_ms"].isInt())) {
    poll_interval_ms = body["poll_interval_ms"].asInt64();
  }
  if (poll_interval_ms < 25) poll_interval_ms = 25;
  if (poll_interval_ms > 5000) poll_interval_ms = 5000;

  int64_t tone_hz = 440;
  if (body.isMember("tone_hz") && (body["tone_hz"].isInt64() || body["tone_hz"].isUInt64() || body["tone_hz"].isInt())) {
    tone_hz = body["tone_hz"].asInt64();
  }
  if (tone_hz < 50) tone_hz = 50;
  if (tone_hz > 4000) tone_hz = 4000;

  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    auto st = voice_peer_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_state(st.get());
    if (st && st->running) {
      out["ok"] = true;
      out["already_running"] = true;
      out["peer"] = voice_peer_runtime_to_json(*st);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *st, &perr);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    if (st && !st->running) g_voice_peer_by_session.erase(session_id);
  }
  {
    std::shared_ptr<VoicePeerRuntime> persisted;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, session_id, &persisted, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (persisted && persisted->running) {
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        g_voice_peer_by_session[session_id] = persisted;
      }
      out["ok"] = true;
      out["already_running"] = true;
      out["peer"] = voice_peer_runtime_to_json(*persisted);
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *persisted, &perr);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
  }

  const std::string broker_url = body.isMember("broker_url") && body["broker_url"].isString() ? trim_copy(body["broker_url"].asString()) : "";
  const std::string broker_session_id =
    body.isMember("broker_session_id") && body["broker_session_id"].isString() ? trim_copy(body["broker_session_id"].asString()) : "";
  const std::string broker_token = body.isMember("broker_token") && body["broker_token"].isString() ? trim_copy(body["broker_token"].asString()) : "";
  std::string sender_tag =
    body.isMember("sender_tag") && body["sender_tag"].isString()
      ? trim_copy(body["sender_tag"].asString())
      : std::string("agentd_runtime_peer");
  if (sender_tag.empty()) sender_tag = "agentd_runtime_peer";
  if (!is_safe_shellish_token(broker_session_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_session_id");
    return;
  }
  if (broker_url.empty() || !is_safe_printable_field(broker_url, 2048)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_url");
    return;
  }
  if (broker_token.empty() || !is_safe_printable_field(broker_token, 1024)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_token");
    return;
  }
  if (!is_safe_shellish_token(sender_tag, 96)) {
    resp->status = 400;
    resp->body = json_error_body("invalid sender_tag");
    return;
  }

#if defined(_WIN32)
  out["error"] = "voice_webrtc_peer start unsupported on Windows";
  resp->status = 501;
  resp->body = json_stringify(out);
  return;
#else
  std::string serr;
  std::string tool_path;
  std::string node_bin;
  if (!resolve_voice_peer_backend(cfg, runtime_kind, &tool_path, &node_bin, &serr)) {
    out["error"] = serr.empty() ? "failed to resolve voice peer backend" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  std::shared_ptr<VoicePeerRuntime> spawned;
  if (!voice_peer_spawn_process(
        cfg,
        runtime_kind,
        tool_path,
        node_bin,
        session_id,
        broker_session_id,
        broker_url,
        broker_token,
        sender_tag,
        deadline_ms,
        poll_interval_ms,
        tone_hz,
        &spawned,
        &serr)) {
    out["error"] = serr.empty() ? "failed to start voice peer" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session[session_id] = spawned;
    out["peer"] = voice_peer_runtime_to_json(*spawned);
  }
  std::string perr;
  (void)persist_voice_peer_runtime_record(db, *spawned, &perr);
  out["ok"] = true;
  out["started"] = true;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

void handle_session_voice_webrtc_peer_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty() || !session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  voice_peer_add_runtime_metadata(cfg, &out);

  std::string err;
  bool session_exists = false;
  if (!db->session_exists(*sid, &session_exists, &err)) {
    resp->status = 500;
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to query session" : err;
    resp->body = json_stringify(out);
    return;
  }
  out["session_exists"] = session_exists;

  std::shared_ptr<VoicePeerRuntime> st;
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    st = voice_peer_lookup_locked(*sid);
    if (st) refresh_voice_peer_runtime_state(st.get());
  }
  if (!st) {
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, *sid, &st, &lerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->body = json_stringify(out);
      return;
    }
    if (st && st->running) {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      g_voice_peer_by_session[*sid] = st;
    }
  }
  if (st) {
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
  }
  out["peer"] = st ? voice_peer_runtime_to_json(*st) : Json::Value(Json::nullValue);
  out["running"] = st ? st->running : false;
  resp->status = 200;
  resp->body = json_stringify(out);
}

}  // namespace agentd
