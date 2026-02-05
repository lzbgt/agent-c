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
  // Edge/MCU interop message authentication (UM-BMP envelope-level auth).
  //
  // Motivation:
  // - Gateways and MCU nodes may share a transport (MQTT, LoRa bridges) where payload authenticity matters.
  // - v0.4 goal: enforceable trust roots and stronger node identity binding.
  //
  // Current minimal mechanism:
  // - HMAC-SHA256 over canonical JSON of the envelope (with `auth` removed), with key selected by `auth.kid`.
  // - Keys are secrets; do not expose via /api/v1/config. Use /api/v1/config/update or local secrets files.
  bool edge_auth_required = false;
  // Optional freshness controls for authenticated envelopes (replay window hardening).
  //
  // - edge_auth_require_ts: when true, authenticated envelopes must include a valid `ts_utc_ms` (> 0).
  // - edge_auth_max_skew_ms: when > 0, authenticated envelopes with ts_utc_ms are rejected if
  //   abs(now - ts_utc_ms) > edge_auth_max_skew_ms.
  //
  // Defaults are permissive for bring-up; operators can tighten once nodes have clocks.
  bool edge_auth_require_ts = false;
  int64_t edge_auth_max_skew_ms = 0;
  // Key selection / identity binding policy.
  //
  // Values:
  // - "any" (default): any configured `kid` is allowed (legacy / gateway mode).
  // - "match_node": for authenticated envelopes with `from:"node:<node_id>"`, require `auth.kid == <node_id>`.
  // - "node_prefix": for authenticated envelopes with `from:"node:<node_id>"`, require
  //    `auth.kid == <node_id>` OR `auth.kid` starts with `<node_id>:` (rotation friendly).
  //
  // Rationale:
  // - "any" is easy for bring-up, but a single shared secret is a large blast radius.
  // - "match_node" / "node_prefix" let operators provision per-node secrets without changing envelope shape.
  std::string edge_auth_kid_policy = "any";
  std::map<std::string, std::string> edge_auth_hmac_keys; // kid -> secret
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
  // Workflow fair-queue policy (explicit scheduling surface; v2.2).
  //
  // - scan_rr: legacy session-aware scan order (round-robin start cursor over session buckets)
  // - wrr: weighted round-robin over session buckets (weights derived from workflow submit spec's `session_weight`)
  // - drr: deficit round-robin over session buckets (v2.3)
  //
  // Note: when weights are all 1 (default), wrr is effectively equivalent to scan_rr.
  std::string workflow_engine_fair_queue_policy = "wrr";
  int workflow_engine_fair_queue_max_session_weight = 16; // clamps per-session weight extracted from spec_json
  int workflow_engine_fair_queue_max_schedule_len = 1024; // bounds expanded WRR session schedule length
  // Optional DRR enhancement: charge deficits by estimated task cost instead of unit cost.
  // Values: "unit" (default), "simple_v1".
  std::string workflow_engine_drr_cost_model = "unit";
  // Workflow admission control (backpressure at submit time).
  //
  // These caps apply in `POST /api/v1/workflow/submit` and are intended to prevent fan-out storms from
  // consuming unbounded DB rows and memory. Caps are best-effort and intentionally simple.
  //
  // - max_inflight_tasks_per_session: caps total workflow_tasks with status queued|running across all workflows
  //   sharing the same session_id (0 disables).
  // - max_inflight_tasks_total: caps total workflow_tasks with status queued|running across the whole daemon (0 disables).
  int workflow_admit_max_inflight_tasks_per_session = 0;
  int workflow_admit_max_inflight_tasks_total = 0;

  // Workflow task surface hardening.
  //
  // `kind:"http_json"` is a deterministic workflow task that can make outbound HTTP requests.
  // This is powerful (enables broker/agent interop) but can be an SSRF footgun if enabled by
  // default. Keep disabled unless an operator explicitly opts in.
  bool workflow_enable_http_tasks = false;
  // Optional outbound HTTP allowlist for deterministic collaboration tasks (`http_json`, `agentd_call`).
  //
  // Semantics:
  // - empty: allow any host (when workflow_enable_http_tasks is enabled)
  // - non-empty: allow only URLs whose parsed host[:port] matches at least one entry
  //
  // Entry format:
  // - "example.com" (any port)
  // - "example.com:443" (specific port)
  // - "127.0.0.1" or "127.0.0.1:9090"
  // - "[::1]" or "[::1]:9090"
  std::vector<std::string> workflow_http_allow_hosts;
  // Optional CIDR allowlist for outbound deterministic HTTP tasks.
  //
  // Entries:
  // - "192.168.0.0/16"
  // - "10.0.0.0/8"
  // - "127.0.0.0/8"
  // - "fd00::/8"
  std::vector<std::string> workflow_http_allow_cidrs;
  // Defense-in-depth SSRF hardening:
  // - when enabled, outbound deterministic HTTP targets that resolve to private/loopback/link-local
  //   addresses are rejected unless explicitly allowed by:
  //   - matching workflow_http_allow_cidrs, or
  //   - matching workflow_http_allow_hosts for a literal IP host target.
  bool workflow_http_deny_private_addrs = false;
  // Optional CIDR denylist for outbound deterministic HTTP tasks.
  //
  // This is checked in addition to allowlist/deny-private and is intended as
  // defense-in-depth against DNS rebinding and configuration mistakes.
  //
  // If any resolved address matches a denied CIDR, the request is rejected.
  std::vector<std::string> workflow_http_deny_cidrs;
  // Optional DNS pinning for outbound deterministic HTTP tasks.
  //
  // When enabled, the HTTP client resolves hostnames for each request and pins
  // the resolved addresses into libcurl via CURLOPT_RESOLVE, preventing DNS
  // rebinding between "allowlist check" and actual connect.
  //
  // Tradeoff: this can reduce the effective "live load balancing" of DNS-based
  // hostnames (each request uses the resolution result captured at request time).
  bool workflow_http_dns_pin = false;

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
