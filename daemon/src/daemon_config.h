#pragma once

#include "sandbox_policy.h"

#include <cstdint>
#include <map>
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
  // Optional provider-specific keys (preferred over a single global api_key when multiple providers are used).
  // Keys are stored only in memory and (optionally) in a local runtime secrets file under state_dir.
  // Never expose these in /api/v1/config responses.
  std::map<std::string, std::string> provider_keys;
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
  // - state_dir: "<daemon startup working directory>" (best-effort; or env AGENT_WD / AGENTD_STATE_DIR)
  // - sessions_root_dir: "<state_dir>" (so session root is "<state_dir>/session_<session_id>/")
  std::string state_dir;
  std::string sessions_root_dir;

  // Optional troubleshooting DB mirror (SQLite). When set, agentd mirrors sessions/runs/events into this DB.
  // As of 2026-01-31, the DB is intended to be the canonical daemon state store.
  // If empty, agentd will default to "<state_dir>/agentd.db".
  std::string db_path;
  std::string tools = "host";     // none|basic|host
  bool yolo_default = true;       // default to unrestricted unless client requests scoped mode
  HostToolsetPolicyMode host_policy = HostToolsetPolicyMode::Full; // host tools: full|readonly
  bool no_default_system = false; // when false, host tool runs insert a default system hint (one time)
  // Host-only: selects which built-in host system prompt to inject when `system` is not provided.
  // See `host_system_prompt_for_profile()` for known values.
  std::string system_profile = "default";
  size_t max_chars_default = 20000;
  size_t keep_last_default = 16;
  // Default tool-loop step limit when requests omit `max_steps`.
  // 0 means unlimited.
  size_t max_steps_default = 0;
  // Default cap on total tool calls when requests omit it.
  // 0 means unlimited.
  size_t max_tool_calls_total_default = 0;
  // Default per-tool cap when requests omit it.
  // 0 means unlimited / disabled.
  size_t max_tool_calls_per_tool_default = 0;

  // Default explicit per-tool tool-call limits when requests omit them.
  // These are applied in addition to (and take precedence over) max_tool_calls_per_tool_default.
  // Empty means "no explicit per-tool caps by default".
  std::vector<std::pair<std::string, size_t>> tool_call_limits_default = {};

  // Async job GC (daemon longevity): finished jobs are kept only for a bounded time/count.
  int64_t job_ttl_ms = 30 * 60 * 1000; // 30 minutes
  size_t max_jobs = 256;

  // Scheduler knobs (high leverage for autonomous efficiency).
  // These control the background job/workflow engines that claim work from SQLite and execute it.
  int job_engine_max_concurrency = 2;
  int job_engine_poll_ms = 200;
  int workflow_engine_max_concurrency = 4;
  int workflow_engine_poll_ms = 200;
  // Workflow fairness/budgets:
  // - max_inflight_per_workflow: prevents a single fan-out workflow from monopolizing all workers
  // - max_inflight_per_session: optional multi-tenant fairness guard (0 = unlimited/disabled)
  int workflow_engine_max_inflight_per_workflow = 2;
  int workflow_engine_max_inflight_per_session = 0;

  // Memory consolidation (rolling, deterministic by default).
  //
  // When enabled, the daemon periodically scans recent daily memory files for explicit @mem markers and
  // promotes them into structured memory (memory/STRUCTURED.md) via an idempotent upsert.
  //
  // Default: disabled (0) to avoid surprising background writes for new users.
  int64_t memory_consolidate_interval_ms = 0;
  int memory_consolidate_daily_days = 14;
  int memory_consolidate_keep_checkpoints = 100;

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
