#include "session_voice_backend_policy.h"

#include "string_util.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <vector>

namespace agentd {
namespace {

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

static std::string bundled_audio_peer_tool_name() {
  return "agentd_audio_webrtc_peer.js";
}

static std::string normalized_path_string(const std::filesystem::path& p) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(p, ec);
  return (ec ? p : abs).lexically_normal().string();
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

static std::string configured_default_voice_peer_runtime_kind(const DaemonConfig& cfg) {
  const std::string kind = lower_copy(trim_copy(cfg.audio_webrtc_default_runtime_kind));
  if (kind == "builtin" || kind == "bundled" || kind == "external") return kind;
  return "";
}

}  // namespace

std::string discover_bundled_audio_peer_tool_path(const DaemonConfig& cfg) {
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

std::string default_voice_peer_runtime_kind_source(const DaemonConfig& cfg) {
  if (configured_default_voice_peer_runtime_kind(cfg).empty()) return "auto";
  return cfg.audio_webrtc_default_runtime_kind_from_env ? "env" : "config";
}

std::string default_voice_peer_runtime_kind(const DaemonConfig& cfg) {
  const std::string configured = configured_default_voice_peer_runtime_kind(cfg);
  if (!configured.empty()) return configured;
  return discover_bundled_audio_peer_tool_path(cfg).empty() ? "external" : "bundled";
}

std::string voice_peer_backend_unavailable_reason(const DaemonConfig& cfg, const std::string& runtime_kind) {
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

bool resolve_voice_peer_backend(
  const DaemonConfig& cfg,
  const std::string& runtime_kind,
  std::string* out_tool_path,
  std::string* out_node_bin,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_tool_path) out_tool_path->clear();
  const std::string kind = lower_copy(trim_copy(runtime_kind));
  const std::string node_bin = trim_copy(
    cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);
  const std::string unavailable_reason = voice_peer_backend_unavailable_reason(cfg, kind);
  if (!unavailable_reason.empty()) {
    if (out_err) *out_err = unavailable_reason;
    return false;
  }
  if (out_node_bin) *out_node_bin = node_bin;
  if (kind == "bundled") {
    if (out_tool_path) *out_tool_path = discover_bundled_audio_peer_tool_path(cfg);
    return true;
  }
  if (kind == "external") {
    if (out_tool_path) *out_tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
    return true;
  }
  if (out_err) *out_err = "runtime_kind must be bundled, external, or builtin";
  return false;
}

Json::Value session_voice_webrtc_backend_metadata_json(const DaemonConfig& cfg) {
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

}  // namespace agentd
