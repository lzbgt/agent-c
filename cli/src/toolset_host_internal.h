#pragma once

#include "toolset_host.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace host_tools_internal {

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
  HostToolsetPolicyMode policy = HostToolsetPolicyMode::Full;
  HostCancelCallback should_cancel = nullptr;
  void* should_cancel_ctx = nullptr;
  GitignoreCache gitignore;
};

inline bool is_cancelled(const HostToolCtx* ctx) {
  return ctx && ctx->should_cancel && ctx->should_cancel(ctx->should_cancel_ctx);
}

// Exec-capable tools (moved to a separate TU to keep files < 2000 LOC).
agent_status_t tool_shell_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_proc_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);
agent_status_t tool_file_apply_patch(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result);

} // namespace host_tools_internal
