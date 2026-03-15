#include "session_voice_runtime.h"

#include "daemon_auth.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <chrono>
#include <cstdlib>
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
  std::string broker_agent_id;
  std::string broker_deployment_id;
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
  bool managed_broker_session = false;
  bool ready = false;
  bool running = false;
  bool suppress_persist = false;
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

struct VoicePeerStartupWaitResult {
  bool ready = false;
  bool running = false;
  bool timed_out = false;
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
  out["managed_broker_session"] = st.managed_broker_session;
  if (!st.broker_agent_id.empty()) out["broker_agent_id"] = st.broker_agent_id;
  if (!st.broker_deployment_id.empty()) out["broker_deployment_id"] = st.broker_deployment_id;
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

static std::string join_base_path(const std::string& base_in, const std::string& suffix) {
  std::string base = trim_copy(base_in);
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (suffix.empty()) return base;
  if (suffix.front() == '/') return base + suffix;
  return base + "/" + suffix;
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

static std::string configured_default_voice_peer_runtime_kind(const DaemonConfig& cfg) {
  const std::string kind = lower_copy(trim_copy(cfg.audio_webrtc_default_runtime_kind));
  if (kind == "bundled" || kind == "external") return kind;
  return "";
}

static std::string default_voice_peer_runtime_kind_source(const DaemonConfig& cfg) {
  if (configured_default_voice_peer_runtime_kind(cfg).empty()) return "auto";
  return cfg.audio_webrtc_default_runtime_kind_from_env ? "env" : "config";
}

static std::string default_voice_peer_runtime_kind(const DaemonConfig& cfg) {
  const std::string configured = configured_default_voice_peer_runtime_kind(cfg);
  if (!configured.empty()) return configured;
  return discover_bundled_audio_peer_tool_path(cfg).empty() ? "external" : "bundled";
}

static bool path_exists_for_exec_probe(const std::string& path_in) {
  const std::string path = trim_copy(path_in);
  if (path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

static bool command_exists_for_exec_probe(const std::string& command_in) {
  const std::string command = trim_copy(command_in);
  if (command.empty()) return false;
  if (command.find('/') != std::string::npos || command.find('\\') != std::string::npos) {
    return path_exists_for_exec_probe(command);
  }

  const char* path_env = std::getenv("PATH");
  if (!path_env || !*path_env) return false;
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string path_value(path_env);
  size_t start = 0;
  while (start <= path_value.size()) {
    size_t end = path_value.find(sep, start);
    const std::string dir = path_value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!dir.empty()) {
      if (path_exists_for_exec_probe((std::filesystem::path(dir) / command).string())) return true;
#if defined(_WIN32)
      if (path_exists_for_exec_probe((std::filesystem::path(dir) / (command + ".exe")).string())) return true;
#endif
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

static std::string voice_peer_backend_unavailable_reason(const DaemonConfig& cfg, const std::string& runtime_kind) {
  const std::string kind = lower_copy(trim_copy(runtime_kind));
  if (kind == "builtin") return "builtin voice_webrtc_peer runtime not implemented";

  const std::string node_bin = trim_copy(
    cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);
  if (!is_safe_shellish_token(node_bin, 256)) return "invalid audio_webrtc_peer_node_bin";
  if (!command_exists_for_exec_probe(node_bin)) return "audio_webrtc_peer_node_bin not found";

  if (kind == "bundled") {
    const std::string bundled = discover_bundled_audio_peer_tool_path(cfg);
    return bundled.empty() ? "bundled voice_webrtc_peer tool unavailable" : "";
  }
  if (kind == "external") {
    const std::string tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
    if (tool_path.empty()) return "audio_webrtc_peer_tool_path not configured";
    return path_exists_for_exec_probe(tool_path) ? "" : "audio_webrtc_peer tool path not found";
  }
  return "runtime_kind must be bundled, external, or builtin";
}

static bool voice_peer_backend_available(const DaemonConfig& cfg, const std::string& runtime_kind) {
  return voice_peer_backend_unavailable_reason(cfg, runtime_kind).empty();
}

static std::string effective_voice_broker_url(const DaemonConfig& cfg, const std::string& request_broker_url) {
  const std::string requested = trim_copy(request_broker_url);
  if (!requested.empty()) return requested;
  return trim_copy(cfg.audio_webrtc_broker_url);
}

static std::string effective_voice_broker_token(const DaemonConfig& cfg, const std::string& request_broker_token) {
  const std::string requested = trim_copy(request_broker_token);
  if (!requested.empty()) return requested;
  return trim_copy(cfg.audio_webrtc_broker_token);
}

static bool validate_voice_broker_token_if_present(const std::string& broker_token, std::string* out_err) {
  if (out_err) out_err->clear();
  if (broker_token.empty()) return true;
  if (!is_safe_printable_field(broker_token, 1024)) {
    if (out_err) *out_err = "invalid configured audio_webrtc_broker_token";
    return false;
  }
  return true;
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
  if (v.isMember("broker_agent_id") && v["broker_agent_id"].isString()) st.broker_agent_id = trim_copy(v["broker_agent_id"].asString());
  if (v.isMember("broker_deployment_id") && v["broker_deployment_id"].isString()) st.broker_deployment_id = trim_copy(v["broker_deployment_id"].asString());
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
  if (v.isMember("managed_broker_session") && v["managed_broker_session"].isBool()) {
    st.managed_broker_session = v["managed_broker_session"].asBool();
  }
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

static bool clear_voice_peer_runtime_record(AgentDb* db, const std::string& session_id, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  return db->meta_set(voice_peer_meta_key(session_id), "", out_err);
}

static bool load_voice_peer_runtime_record(
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  bool* out_self_healed,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_self_healed) *out_self_healed = false;
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
    std::string cerr;
    if (!clear_voice_peer_runtime_record(db, session_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? (jerr.empty() ? "persisted voice runtime record corrupt" : jerr)
          : ("failed to clear corrupt persisted voice runtime record: " + cerr);
      }
      return false;
    }
    if (out_self_healed) *out_self_healed = true;
    return true;
  }
  auto st = std::make_shared<VoicePeerRuntime>();
  if (!voice_peer_runtime_from_json(parsed, st.get(), out_err)) {
    const std::string original_err = out_err ? *out_err : std::string("persisted voice runtime record corrupt");
    std::string cerr;
    if (!clear_voice_peer_runtime_record(db, session_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? original_err
          : ("failed to clear corrupt persisted voice runtime record: " + cerr);
      }
      return false;
    }
    if (out_state) out_state->reset();
    if (out_self_healed) *out_self_healed = true;
    if (out_err) out_err->clear();
    return true;
  }
  st->status_source = "persisted";
  refresh_voice_peer_runtime_state(st.get());
  if (out_state) *out_state = std::move(st);
  return true;
}

static Json::Value session_voice_webrtc_backend_metadata_json_impl(const DaemonConfig& cfg) {
  const std::string bundled_tool_path = discover_bundled_audio_peer_tool_path(cfg);
  const std::string default_runtime_kind = default_voice_peer_runtime_kind(cfg);
  const std::string builtin_reason = voice_peer_backend_unavailable_reason(cfg, "builtin");
  const std::string bundled_reason = voice_peer_backend_unavailable_reason(cfg, "bundled");
  const std::string external_reason = voice_peer_backend_unavailable_reason(cfg, "external");
  const std::string default_reason = voice_peer_backend_unavailable_reason(cfg, default_runtime_kind);
  Json::Value out(Json::objectValue);
  out["builtin_available"] = false;
  out["bundled_available"] = bundled_reason.empty();
  out["external_available"] = external_reason.empty();
  out["default_runtime_kind"] = default_runtime_kind;
  out["default_runtime_kind_source"] = default_voice_peer_runtime_kind_source(cfg);
  out["default_runtime_kind_available"] = default_reason.empty();
  out["tool_configured"] = !trim_copy(cfg.audio_webrtc_peer_tool_path).empty();
  out["broker_url_default_configured"] = !trim_copy(cfg.audio_webrtc_broker_url).empty();
  out["broker_token_default_configured"] = !trim_copy(cfg.audio_webrtc_broker_token).empty();
  if (!builtin_reason.empty()) out["builtin_unavailable_reason"] = builtin_reason;
  if (!bundled_reason.empty()) out["bundled_unavailable_reason"] = bundled_reason;
  if (!external_reason.empty()) out["external_unavailable_reason"] = external_reason;
  if (!default_reason.empty()) out["default_runtime_kind_unavailable_reason"] = default_reason;
  if (!cfg.audio_webrtc_peer_tool_path.empty()) out["tool_path"] = cfg.audio_webrtc_peer_tool_path;
  if (!bundled_tool_path.empty()) out["bundled_tool_path"] = bundled_tool_path;
  out["node_bin"] = cfg.audio_webrtc_peer_node_bin.empty() ? "node" : cfg.audio_webrtc_peer_node_bin;
  return out;
}

static void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out) {
  if (!out) return;
  const Json::Value meta = session_voice_webrtc_backend_metadata_json_impl(cfg);
  for (const auto& name : meta.getMemberNames()) {
    (*out)[name] = meta[name];
  }
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
  const std::string unavailable_reason = voice_peer_backend_unavailable_reason(cfg, kind);
  if (!unavailable_reason.empty()) {
    if (out_err) *out_err = unavailable_reason;
    return false;
  }
  if (out_node_bin) *out_node_bin = node_bin;
  if (kind == "bundled") {
    const std::string bundled = discover_bundled_audio_peer_tool_path(cfg);
    if (out_tool_path) *out_tool_path = bundled;
    return true;
  }
  if (kind == "external") {
    const std::string tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
    if (out_tool_path) *out_tool_path = tool_path;
    return true;
  }
  if (out_err) *out_err = "runtime_kind must be bundled, external, or builtin";
  return false;
}

static bool broker_create_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_agent_id,
  const std::string& broker_deployment_id,
  std::string* out_session_id,
  std::string* out_err
) {
  if (out_session_id) out_session_id->clear();
  if (out_err) out_err->clear();

  Json::Value body(Json::objectValue);
  body["agent_id"] = broker_agent_id;
  body["mode"] = "webrtc";
  if (!broker_deployment_id.empty()) body["deployment_id"] = broker_deployment_id;

  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions"),
    "POST",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
      {"Content-Type", "application/json"},
    },
    json_stringify(body),
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/256 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker create request failed" : result.error;
    return false;
  }

  Json::Value parsed(Json::nullValue);
  std::string jerr;
  const bool have_json = json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject();
  if (result.http_status != 200) {
    std::string err = "broker create audio session failed";
    if (have_json && parsed.isMember("error") && parsed["error"].isString()) err += ": " + parsed["error"].asString();
    else err += ": http " + std::to_string(result.http_status);
    if (out_err) *out_err = err;
    return false;
  }
  if (!have_json) {
    if (out_err) *out_err = jerr.empty() ? "broker create response invalid" : jerr;
    return false;
  }
  if (!parsed.isMember("ok") || !parsed["ok"].asBool()) {
    if (out_err) *out_err = "broker create audio session returned ok=false";
    return false;
  }
  if (!parsed.isMember("session_id") || !parsed["session_id"].isString()) {
    if (out_err) *out_err = "broker create response missing session_id";
    return false;
  }
  const std::string session_id = trim_copy(parsed["session_id"].asString());
  if (!is_safe_shellish_token(session_id, 160)) {
    if (out_err) *out_err = "broker create returned invalid session_id";
    return false;
  }
  if (out_session_id) *out_session_id = session_id;
  return true;
}

static bool broker_delete_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (trim_copy(broker_session_id).empty()) return true;
  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions/" + broker_session_id),
    "DELETE",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
    },
    "",
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/128 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker delete request failed" : result.error;
    return false;
  }
  if (result.http_status == 404) return true;
  if (result.http_status != 200) {
    std::string err = "broker delete audio session failed: http " + std::to_string(result.http_status);
    Json::Value parsed(Json::nullValue);
    std::string jerr;
    if (json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject() &&
        parsed.isMember("error") && parsed["error"].isString()) {
      err += " " + parsed["error"].asString();
    }
    if (out_err) *out_err = err;
    return false;
  }
  return true;
}

static bool broker_audio_session_exists(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  bool* out_exists,
  std::string* out_mode,
  std::string* out_err
) {
  if (out_exists) *out_exists = false;
  if (out_mode) out_mode->clear();
  if (out_err) out_err->clear();
  const std::string session_id = trim_copy(broker_session_id);
  if (session_id.empty()) {
    if (out_err) *out_err = "broker_session_id required";
    return false;
  }
  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions/" + session_id),
    "GET",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
    },
    "",
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/128 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker inspect request failed" : result.error;
    return false;
  }
  if (result.http_status == 404) {
    if (out_exists) *out_exists = false;
    return true;
  }
  if (result.http_status != 200) {
    std::string err = "broker inspect audio session failed: http " + std::to_string(result.http_status);
    Json::Value parsed(Json::nullValue);
    std::string jerr;
    if (json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject() &&
        parsed.isMember("error") && parsed["error"].isString()) {
      err += " " + parsed["error"].asString();
    }
    if (out_err) *out_err = err;
    return false;
  }
  Json::Value parsed(Json::nullValue);
  std::string jerr;
  if (!json_parse_any(result.response_body, &parsed, &jerr) || !parsed.isObject()) {
    if (out_err) *out_err = jerr.empty() ? "broker inspect response invalid" : jerr;
    return false;
  }
  if (!parsed.isMember("ok") || !parsed["ok"].asBool()) {
    if (out_err) *out_err = "broker inspect audio session returned ok=false";
    return false;
  }
  if (!parsed.isMember("session") || !parsed["session"].isObject()) {
    if (out_err) *out_err = "broker inspect response missing session";
    return false;
  }
  const Json::Value& session = parsed["session"];
  if (out_mode && session.isMember("mode") && session["mode"].isString()) {
    *out_mode = lower_copy(trim_copy(session["mode"].asString()));
  }
  if (out_exists) *out_exists = true;
  return true;
}

#if !defined(_WIN32)
static bool wait_for_voice_peer_stop(const std::shared_ptr<VoicePeerRuntime>& st, int64_t timeout_ms) {
  if (!st) return true;
  const int64_t deadline = now_unix_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    refresh_voice_peer_runtime_state(st.get());
    return !st->running;
  }
}

static VoicePeerStartupWaitResult wait_for_voice_peer_startup(
  const std::shared_ptr<VoicePeerRuntime>& st,
  int64_t timeout_ms
) {
  VoicePeerStartupWaitResult result;
  if (!st) return result;
  const int64_t deadline = now_unix_ms() + (timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      result.ready = st->ready;
      result.running = st->running;
      if (result.ready || !result.running) return result;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    refresh_voice_peer_runtime_state(st.get());
    result.ready = st->ready;
    result.running = st->running;
    result.timed_out = !result.ready && result.running;
  }
  return result;
}

static bool voice_peer_spawn_process(
  AgentDb* db,
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

  std::thread([db, st]() {
    int status = 0;
    (void)waitpid(st->pid, &status, 0);
    bool should_persist = false;
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      st->running = false;
      st->ready = std::filesystem::exists(st->ready_file_path);
      st->ended_unix_ms = now_unix_ms();
      if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
      should_persist = !st->suppress_persist;
    }
    if (should_persist && db && !trim_copy(st->session_id).empty()) {
      std::string perr;
      (void)persist_voice_peer_runtime_record(db, *st, &perr);
    }
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

static bool remove_voice_peer_runtime_artifacts(
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

}  // namespace

Json::Value session_voice_webrtc_backend_metadata_json(const DaemonConfig& cfg) {
  return session_voice_webrtc_backend_metadata_json_impl(cfg);
}

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

  const std::string request_broker_token =
    body.isMember("broker_token") && body["broker_token"].isString() ? trim_copy(body["broker_token"].asString()) : "";
  if (!request_broker_token.empty() && !is_safe_printable_field(request_broker_token, 1024)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_token");
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
      bool record_self_healed = false;
      std::string lerr;
      if (!load_voice_peer_runtime_record(db, session_id, &st, &record_self_healed, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      if (record_self_healed) {
        Json::Value cleanup(Json::objectValue);
        cleanup["persisted_record_cleared"] = true;
        bool artifacts_deleted = false;
        std::string aerr;
        if (remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
          cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
        } else if (!aerr.empty()) {
          cleanup["runtime_artifacts_delete_error"] = aerr;
        }
        out["cleanup_on_corrupt_record"] = cleanup;
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
    const bool stopped = wait_for_voice_peer_stop(st, 2000);
    std::string cleanup_err;
    bool cleanup_attempted = false;
    bool cleanup_deleted = false;
    const std::string broker_token = effective_voice_broker_token(cfg, request_broker_token);
    if (st->managed_broker_session && !trim_copy(st->broker_session_id).empty() && !broker_token.empty()) {
      cleanup_attempted = true;
      if (validate_voice_broker_token_if_present(broker_token, &cleanup_err)) {
        cleanup_deleted = broker_delete_audio_session(st->broker_url, broker_token, st->broker_session_id, &cleanup_err);
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      out["peer"] = voice_peer_runtime_to_json(*st);
    }
    std::string perr;
    (void)persist_voice_peer_runtime_record(db, *st, &perr);
    out["ok"] = true;
    out["stopped"] = stopped;
    if (cleanup_attempted) {
      out["broker_session_deleted"] = cleanup_deleted;
      if (!cleanup_deleted && !cleanup_err.empty()) out["broker_session_delete_error"] = cleanup_err;
    }
    resp->status = 200;
    resp->body = json_stringify(out);
    return;
#endif
  }

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

  int64_t startup_wait_ms = 2000;
  if (body.isMember("startup_wait_ms") &&
      (body["startup_wait_ms"].isInt64() || body["startup_wait_ms"].isUInt64() || body["startup_wait_ms"].isInt())) {
    startup_wait_ms = body["startup_wait_ms"].asInt64();
  }
  if (startup_wait_ms < 0) startup_wait_ms = 0;
  if (startup_wait_ms > 10000) startup_wait_ms = 10000;

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
    bool record_self_healed = false;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, session_id, &persisted, &record_self_healed, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (record_self_healed) {
      Json::Value cleanup(Json::objectValue);
      cleanup["persisted_record_cleared"] = true;
      bool artifacts_deleted = false;
      std::string aerr;
      if (remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
        cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
      } else if (!aerr.empty()) {
        cleanup["runtime_artifacts_delete_error"] = aerr;
      }
      out["cleanup_on_corrupt_record"] = cleanup;
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

  const std::string request_broker_url =
    body.isMember("broker_url") && body["broker_url"].isString() ? trim_copy(body["broker_url"].asString()) : "";
  const std::string requested_broker_session_id =
    body.isMember("broker_session_id") && body["broker_session_id"].isString() ? trim_copy(body["broker_session_id"].asString()) : "";
  const std::string broker_agent_id =
    body.isMember("broker_agent_id") && body["broker_agent_id"].isString() ? trim_copy(body["broker_agent_id"].asString()) : "";
  const std::string broker_deployment_id =
    body.isMember("broker_deployment_id") && body["broker_deployment_id"].isString()
      ? trim_copy(body["broker_deployment_id"].asString())
      : "";
  std::string sender_tag =
    body.isMember("sender_tag") && body["sender_tag"].isString()
      ? trim_copy(body["sender_tag"].asString())
      : std::string("agentd_runtime_peer");
  if (sender_tag.empty()) sender_tag = "agentd_runtime_peer";
  if (!requested_broker_session_id.empty() && !is_safe_shellish_token(requested_broker_session_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_session_id");
    return;
  }
  if (!broker_agent_id.empty() && !is_safe_shellish_token(broker_agent_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_agent_id");
    return;
  }
  if (!broker_deployment_id.empty() && !is_safe_shellish_token(broker_deployment_id, 160)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_deployment_id");
    return;
  }
  if (!request_broker_url.empty() && !is_safe_printable_field(request_broker_url, 2048)) {
    resp->status = 400;
    resp->body = json_error_body("invalid broker_url");
    return;
  }
  if (!requested_broker_session_id.empty() && (!broker_agent_id.empty() || !broker_deployment_id.empty())) {
    resp->status = 400;
    resp->body = json_error_body("broker_agent_id and broker_deployment_id must be omitted when broker_session_id is provided");
    return;
  }
  const std::string broker_url = effective_voice_broker_url(cfg, request_broker_url);
  if (broker_url.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_url required when daemon default not configured");
    return;
  }
  if (!is_safe_printable_field(broker_url, 2048)) {
    out["error"] = "invalid configured audio_webrtc_broker_url";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  const std::string broker_token = effective_voice_broker_token(cfg, request_broker_token);
  if (!validate_voice_broker_token_if_present(broker_token, &err)) {
    out["error"] = err;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  if (broker_token.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_token required when daemon default not configured");
    return;
  }
  if (requested_broker_session_id.empty() && broker_agent_id.empty()) {
    resp->status = 400;
    resp->body = json_error_body("broker_agent_id required when broker_session_id omitted");
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

  std::string broker_session_id = requested_broker_session_id;
  bool managed_broker_session = false;
  if (broker_session_id.empty()) {
    if (!broker_create_audio_session(
          broker_url,
          broker_token,
          broker_agent_id,
          broker_deployment_id,
          &broker_session_id,
          &serr)) {
      out["error"] = serr.empty() ? "failed to create broker audio session" : serr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    managed_broker_session = true;
  } else {
    bool session_exists = false;
    std::string broker_session_mode;
    if (!broker_audio_session_exists(broker_url, broker_token, broker_session_id, &session_exists, &broker_session_mode, &serr)) {
      out["error"] = serr.empty() ? "failed to inspect broker audio session" : serr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (!session_exists) {
      resp->status = 400;
      resp->body = json_error_body("broker_session_id not found");
      return;
    }
    if (!broker_session_mode.empty() && broker_session_mode != "webrtc") {
      resp->status = 400;
      resp->body = json_error_body("broker_session_id mode must be webrtc");
      return;
    }
  }

  std::shared_ptr<VoicePeerRuntime> spawned;
  if (!voice_peer_spawn_process(
        db,
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
    if (managed_broker_session) {
      std::string derr;
      if (!broker_delete_audio_session(broker_url, broker_token, broker_session_id, &derr) && serr.empty()) serr = derr;
    }
    out["error"] = serr.empty() ? "failed to start voice peer" : serr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  spawned->managed_broker_session = managed_broker_session;
  spawned->broker_agent_id = broker_agent_id;
  spawned->broker_deployment_id = broker_deployment_id;
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session[session_id] = spawned;
  }
  std::string perr;
  (void)persist_voice_peer_runtime_record(db, *spawned, &perr);
  const VoicePeerStartupWaitResult startup = wait_for_voice_peer_startup(spawned, startup_wait_ms);
  if (!startup.running && !startup.ready) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_session_voice_webrtc_peer_runtime(cfg, db, session_id, broker_token, &cleanup, &cerr)) {
      out["error"] = cerr.empty() ? "voice peer exited before ready and cleanup failed" : cerr;
      {
        std::lock_guard<std::mutex> lk(g_voice_peer_mu);
        refresh_voice_peer_runtime_state(spawned.get());
        out["peer"] = voice_peer_runtime_to_json(*spawned);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::string startup_err = trim_copy(spawned->last_error);
    if (startup_err.empty()) startup_err = "voice peer exited before ready";
    out["error"] = startup_err;
    out["startup_confirmed"] = false;
    out["startup_cleanup"] = cleanup;
    if (cleanup.isMember("broker_session_deleted")) out["broker_session_deleted"] = cleanup["broker_session_deleted"];
    if (cleanup.isMember("broker_session_delete_error")) {
      out["broker_session_delete_error"] = cleanup["broker_session_delete_error"];
    }
    out["peer"] = cleanup.isMember("peer") ? cleanup["peer"] : Json::Value(Json::nullValue);
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    refresh_voice_peer_runtime_state(spawned.get());
    out["peer"] = voice_peer_runtime_to_json(*spawned);
  }
  out["ok"] = true;
  out["started"] = true;
  out["startup_confirmed"] = startup.ready;
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
    bool record_self_healed = false;
    std::string lerr;
    if (!load_voice_peer_runtime_record(db, *sid, &st, &record_self_healed, &lerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = lerr.empty() ? "failed to load persisted voice peer state" : lerr;
      resp->body = json_stringify(out);
      return;
    }
    if (record_self_healed) {
      Json::Value cleanup(Json::objectValue);
      cleanup["persisted_record_cleared"] = true;
      bool artifacts_deleted = false;
      std::string aerr;
      if (remove_voice_peer_runtime_artifacts(cfg, *sid, &artifacts_deleted, &aerr)) {
        cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
      } else if (!aerr.empty()) {
        cleanup["runtime_artifacts_delete_error"] = aerr;
      }
      out["cleanup_on_corrupt_record"] = cleanup;
    }
    if (st && st->running) {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      g_voice_peer_by_session[*sid] = st;
    }
  }
  if (!session_exists && st) {
    Json::Value cleanup(Json::objectValue);
    std::string cerr;
    if (!cleanup_session_voice_webrtc_peer_runtime(cfg, db, *sid, "", &cleanup, &cerr)) {
      resp->status = 500;
      out["ok"] = false;
      out["error"] = cerr.empty() ? "failed to clean up stale voice peer state" : cerr;
      if (!cleanup.empty()) out["cleanup_on_missing_session"] = cleanup;
      resp->body = json_stringify(out);
      return;
    }
    out["cleanup_on_missing_session"] = cleanup;
    st.reset();
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
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    st = voice_peer_lookup_locked(session_id);
    if (st) refresh_voice_peer_runtime_state(st.get());
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
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      st->suppress_persist = true;
    }
    summary["runtime_present"] = true;
    summary["runtime_was_running"] = st->running;
    summary["peer"] = voice_peer_runtime_to_json(*st);
  }

#if defined(_WIN32)
  if (st && st->running) {
    if (out_err) *out_err = "voice_webrtc_peer cleanup unsupported on Windows";
    return false;
  }
#else
  if (st && st->running) {
    std::string serr;
    if (!voice_peer_kill_best_effort(st, &serr)) {
      if (out_err) *out_err = serr.empty() ? "failed to stop voice peer during session delete" : serr;
      return false;
    }
    summary["stopped"] = wait_for_voice_peer_stop(st, 2000);
    {
      std::lock_guard<std::mutex> lk(g_voice_peer_mu);
      refresh_voice_peer_runtime_state(st.get());
      summary["peer"] = voice_peer_runtime_to_json(*st);
    }
  } else {
    summary["stopped"] = st ? !st->running : false;
  }
#endif

  bool broker_deleted = false;
  const std::string broker_token_effective = effective_voice_broker_token(cfg, broker_token);
  if (st && st->managed_broker_session && !trim_copy(st->broker_session_id).empty() && !broker_token_effective.empty()) {
    std::string berr;
    summary["broker_session_delete_attempted"] = true;
    if (validate_voice_broker_token_if_present(broker_token_effective, &berr)) {
      broker_deleted = broker_delete_audio_session(st->broker_url, broker_token_effective, st->broker_session_id, &berr);
    }
    summary["broker_session_deleted"] = broker_deleted;
    if (!broker_deleted && !berr.empty()) summary["broker_session_delete_error"] = berr;
  } else {
    summary["broker_session_delete_attempted"] = false;
  }

  {
    std::lock_guard<std::mutex> lk(g_voice_peer_mu);
    g_voice_peer_by_session.erase(session_id);
  }

  std::string perr;
  if (!clear_voice_peer_runtime_record(db, session_id, &perr)) {
    if (out_err) *out_err = perr.empty() ? "failed to clear persisted voice peer state" : perr;
    return false;
  }
  summary["persisted_record_cleared"] = true;

  bool artifacts_deleted = false;
  std::string aerr;
  if (!remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
    if (out_err) *out_err = aerr.empty() ? "failed to remove voice peer runtime artifacts" : aerr;
    return false;
  }
  summary["runtime_artifacts_deleted"] = artifacts_deleted;

  if (out_summary) *out_summary = std::move(summary);
  return true;
}

}  // namespace agentd
