#pragma once

#include "sandbox_policy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

struct DaemonConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8123;
  // Optional daemon auth (control-plane). When set, all endpoints (except /health) require
  // Authorization: Bearer <token>. This is distinct from provider API keys.
  std::string auth_token;
  // Safety guard: when binding to a non-loopback host (0.0.0.0, LAN IP), refuse to start unless auth is enabled,
  // unless explicitly overridden (insecure).
  bool allow_unauthenticated_non_loopback = false;
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string model = "gpt-4o-mini";
  std::string summary_model;  // optional: model used to summarize dropped messages during compaction (tools=none)
  size_t summary_max_chars = 1200;
  std::string proxy_url; // optional explicit proxy override (else env)
  long timeout_ms = 60000;

  // State directories (host-only):
  // - state_dir: base directory for daemon state (future: caches, etc.)
  // - sessions_root_dir: directory containing <session>.sess / <session>.events.jsonl
  //
  // Defaults:
  // - state_dir: "~/.agent" (best-effort HOME-based)
  // - sessions_root_dir: "<state_dir>/sessions"
  std::string state_dir;
  std::string sessions_root_dir;

  // Optional troubleshooting DB mirror (SQLite). When set, agentd mirrors sessions/runs/events into this DB.
  // Empty means disabled.
  std::string db_path;
  bool db_disabled = false; // set by --no-db to ignore AGENTD_DB_PATH
  std::string tools = "host";     // none|basic|host
  std::string tools_root = "";    // empty => CWD (unrestricted file edits)
  std::string host_scope_root;    // default: daemon process CWD (for "@host" tool root mode)
  bool yolo_default = true;       // default to unrestricted unless client requests scoped mode
  HostToolsetPolicyMode host_policy = HostToolsetPolicyMode::Full; // host tools: full|readonly
  bool no_default_system = false; // when false, host tool runs insert a default system hint (one time)
  size_t max_chars_default = 20000;
  size_t keep_last_default = 16;
  // Default tool-loop step limit when requests omit `max_steps`.
  // 0 means unlimited (not recommended for long-running daemons).
  size_t max_steps_default = 32;
  // Default cap on total tool calls when requests omit it.
  // 0 means unlimited.
  size_t max_tool_calls_total_default = 128;
  // Default per-tool cap when requests omit it.
  // 0 means unlimited / disabled.
  size_t max_tool_calls_per_tool_default = 0;

  // Default explicit per-tool tool-call limits when requests omit them.
  // These are applied in addition to (and take precedence over) max_tool_calls_per_tool_default.
  //
  // Recommended defaults aim to bound high-risk/high-noise tools without breaking benign workflows.
  // Operators can override via flags/env.
  std::vector<std::pair<std::string, size_t>> tool_call_limits_default = {
    {"proc_exec", 4},
    {"shell_exec", 16},
    // UI/media signaling can also be spammed in runaway loops; keep it bounded by default.
    {"artifact_register", 16},
    {"ui_action", 16},
    // Single-shot tool: keep it extremely bounded by default. Override per-run if you need more frames.
    {"camera_capture", 1},
  };

  // Async job GC (daemon longevity): finished jobs are kept only for a bounded time/count.
  int64_t job_ttl_ms = 30 * 60 * 1000; // 30 minutes
  size_t max_jobs = 256;

  // CORS (for browser-based clients). Defaults depend on listen host:
  // - loopback: allow any origin ("*") for local UI dev
  // - non-loopback: disabled unless explicitly configured via --cors-origin
  bool cors_origins_set = false;
  bool cors_disabled = false;
  std::vector<std::string> cors_origins; // values are exact match, or "*"
  std::string cors_allow_headers;
  std::string cors_allow_methods;
  int cors_max_age_seconds = 600;
};

}  // namespace agentd
