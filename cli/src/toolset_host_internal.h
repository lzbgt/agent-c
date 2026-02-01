#pragma once

#include "toolset_host.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace host_tools_internal {

inline std::string to_generic_string(const std::filesystem::path& p) {
  // Prefer a stable "/"-separated form for JSON/tool output.
  return p.generic_string();
}

inline bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& p) {
  auto it_r = root.begin();
  auto it_p = p.begin();
  for (; it_r != root.end(); ++it_r, ++it_p) {
    if (it_p == p.end()) return false;
    if (*it_r != *it_p) return false;
  }
  return true;
}

inline bool is_symlink_path(const std::filesystem::path& p) {
  std::error_code ec;
  const auto st = std::filesystem::symlink_status(p, ec);
  if (ec) return false;
  return std::filesystem::is_symlink(st);
}

inline bool path_contains_symlink_component(const std::filesystem::path& root, const std::filesystem::path& p) {
  // Assume p is under root (lexically). Walk components and reject if any existing prefix is a symlink.
  std::filesystem::path cur = root;
  // IMPORTANT: use lexically_relative, not std::filesystem::relative, because relative() may resolve
  // symlinks and "erase" the very symlink component we want to detect.
  std::filesystem::path rel = p.lexically_relative(root);
  if (rel.empty()) return false;
  for (const auto& comp : rel) {
    cur /= comp;
    std::error_code ec;
    std::filesystem::file_status st = std::filesystem::symlink_status(cur, ec);
    if (ec) {
      // If a prefix doesn't exist, stop checking further prefixes (can't be a symlink).
      break;
    }
    if (std::filesystem::is_symlink(st)) {
      return true;
    }
  }
  return false;
}

inline std::optional<std::filesystem::path> resolve_under_root(
  const std::filesystem::path& root,
  const std::string& user_path,
  bool unrestricted
) {
  std::filesystem::path p(user_path);
  if (!unrestricted && p.is_absolute()) {
    return std::nullopt;
  }
  std::filesystem::path resolved = p.is_absolute() ? p : (root / p);
  resolved = resolved.lexically_normal();
  if (!unrestricted) {
    const std::filesystem::path norm_root = root.lexically_normal();
    if (!path_is_within(norm_root, resolved)) {
      return std::nullopt;
    }
  }
  return resolved;
}

inline int64_t file_time_to_unix_ms(std::filesystem::file_time_type ft) {
  using file_clock = std::filesystem::file_time_type::clock;
  using sys_clock = std::chrono::system_clock;
  const auto sctp = std::chrono::time_point_cast<sys_clock::duration>(ft - file_clock::now() + sys_clock::now());
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();
}

#if defined(AGENT_HAVE_JSONCPP)
inline bool parse_json(const char* arguments_json, Json::Value* out, std::string* out_err) {
  if (out_err) {
    out_err->clear();
  }
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(arguments_json ? arguments_json : "");
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) {
      *out_err = "invalid JSON: " + errs;
    }
    return false;
  }
  *out = v;
  return true;
}
#endif

inline agent_status_t set_result(agent_string_t* out, const std::string& s) {
  return agent_string_set_copy(out, s.c_str(), s.size());
}

struct GitignoreRule {
  bool negated = false;
  bool subtree = false;   // pattern ended with '/'
  bool anchored = false;  // pattern started with '/'
  bool has_slash = false; // pattern contains '/'
  std::string pattern;
};

struct GitignoreCache {
  bool ready = false;
  std::filesystem::path root; // directory containing .gitignore we loaded
  std::filesystem::path file; // root/.gitignore
  int64_t mtime_unix_ms = 0;
  std::vector<GitignoreRule> rules;
};

struct HostToolCtx {
  std::filesystem::path root;
  bool unrestricted = false;
  bool exec_enabled = true;
  bool allow_symlinks = true;
  HostToolsetPolicyMode policy = HostToolsetPolicyMode::Full;
  HostCancelCallback should_cancel = nullptr;
  void* should_cancel_ctx = nullptr;
  // Optional session context (daemon runs). When set, tools like `ui_wait_event`
  // can read session-scoped UI client events.
  std::filesystem::path sessions_root_dir;
  std::string session_id;
  // Optional: prefer DB-backed client events (agentd canonical state).
  HostReadClientEventsTailCallback read_client_events_tail_cb = nullptr;
  void* read_client_events_tail_ctx = nullptr;
  GitignoreCache gitignore;
};

inline bool is_cancelled(const HostToolCtx* ctx) {
  return ctx && ctx->should_cancel && ctx->should_cancel(ctx->should_cancel_ctx);
}

// Exec-capable tools (moved to a separate TU to keep files < 2000 LOC).
agent_status_t tool_shell_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_proc_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_file_apply_patch(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);

// Artifact/UI signaling tool(s) (also in separate TU).
agent_status_t tool_artifact_register(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_scene_apply(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_ui_action(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_ui_wait_event(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_ui_wait_any(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_ui_wait_all(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_client_wait_event(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_client_wait_any(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_client_wait_all(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_client_peek(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_memory_write(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_memory_get(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_memory_search(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_memory_put(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);

} // namespace host_tools_internal
