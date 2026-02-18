#pragma once

#include "sandbox_policy.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace agentd {

struct CorsRouteConfig {
  std::string path_prefix;
  std::vector<std::string> origins;
};

struct DaemonConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8123;
  // Optional daemon auth (control-plane). When set, all endpoints (except /health) require
  // Authorization: Bearer <token>. This is distinct from provider API keys.
  std::string auth_token;
  // Optional cookie name for daemon auth. When set, a cookie with this name may also supply the auth token.
  // Cookie values may be either the raw token or "Bearer <token>".
  std::string auth_cookie_name;
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
  // - HMAC-SHA256 over the envelope with `auth.sig` removed (auth metadata remains signed),
  //   with key selected by `auth.kid`.
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
  // Optional anti-replay monotonic sequence number.
  // When true, authenticated envelopes must include `auth.seq` (uint64/int64 >= 0), and the platform rejects
  // messages whose seq is not strictly greater than the last accepted seq for that node (best-effort).
  bool edge_auth_require_seq = false;
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
  // Optional public-key trust roots for UM-BMP envelope auth.
  //
  // Map: kid -> base64(pubkey32) where pubkey32 is an Ed25519 public key (32 raw bytes).
  //
  // These are not secrets, but we store them in the runtime secrets blob to keep the control-plane
  // surface uniform (and to avoid accidentally turning /api/v1/config into a directory of node keys).
  std::map<std::string, std::string> edge_auth_ed25519_pubkeys; // kid -> base64(pubkey32)
  // Optional attestation enforcement for edge TASK_DONE results (UM-EAIS v0.x).
  //
  // When enabled, agentd enforces that invoke-mode edge tasks include a `result.attest` blob
  // with a matching `result_sha256`, and (optionally) a valid signature.
  bool edge_attest_required = false;
  bool edge_attest_require_sig = false;
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
  // Session upload limit (per-file, decoded bytes). 0 means "no explicit per-file limit".
  size_t upload_max_bytes = 32 * 1024 * 1024;
  // Blob object store (S3/MinIO) configuration.
  //
  // Modes:
  // - local (default): store blobs in the local filesystem only.
  // - object: store blobs in an object store (optionally keep local cache based on cache_mode).
  std::string blob_store_mode = "local";
  std::string blob_store_endpoint; // e.g. https://s3.us-east-1.amazonaws.com or http://localhost:9000
  std::string blob_store_region = "us-east-1";
  std::string blob_store_bucket;
  std::string blob_store_prefix = "blobs/sha256";
  bool blob_store_path_style = true; // path-style bucket addressing (MinIO-friendly)
  // Read behavior when tier=object and local blob is missing.
  // - redirect: 302 to presigned URL
  // - proxy: agentd fetches the object and returns bytes (bounded)
  std::string blob_store_read_mode = "redirect";
  // Cache behavior for object tier.
  // - none: do not keep local cache on upload or read
  // - read-through: fetch missing objects into local cache (bounded)
  std::string blob_store_cache_mode = "read-through";
  size_t blob_store_cache_max_bytes = 32 * 1024 * 1024;
  int64_t blob_store_presign_ttl_sec = 900;
  int64_t blob_store_timeout_ms = 60000;
  // Secrets (runtime-only; do not expose via /api/v1/config).
  std::string blob_store_access_key;
  std::string blob_store_secret_key;
  std::string blob_store_session_token;
  // Tiering policy (operator-driven).
  // - local_max_bytes: max total bytes for object-tier local cache (0 disables).
  // - local_max_age_ms: evict object-tier local cache older than this age (0 disables).
  // - promote_after_ms: promote local-tier blobs to object store when older than this age (0 disables).
  // - promote_max_bytes: per-blob size cap for promotion (0 disables).
  int64_t blob_tier_local_max_bytes = 0;
  int64_t blob_tier_local_max_age_ms = 0;
  int64_t blob_tier_promote_after_ms = 0;
  int64_t blob_tier_promote_max_bytes = 0;

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
  // Default cap for tool call argument JSON length (per tool call).
  // 0 means unlimited / disabled.
  size_t max_tool_call_args_chars_default = 0;

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
  int64_t memory_retention_interval_ms = 0;
  int memory_retention_daily_max_days = 0;
  int64_t memory_retention_daily_max_bytes = 0;
  int memory_retention_checkpoint_max_days = 0;
  int memory_retention_checkpoint_max_count = 0;
  int memory_retention_structured_deprecate_days = 0; // 0 disables structured deprecate pass
  int memory_retention_structured_deprecate_max_entries = 50;
  int memory_salience_daily_days = 7;
  int memory_salience_max_items = 12;
  int memory_salience_structured_max_items = 6;
  int memory_salience_daily_max_items = 6;
  double memory_salience_half_life_days = 14.0;
  double memory_salience_importance_weight = 0.35;

  // OTA update (disabled by default; requires explicit operator enable + command).
  bool ota_enable = false;
  std::string ota_command; // external command to apply update (see docs/spec/ota/agentd_ota_v0.md)
  int64_t ota_command_timeout_ms = 5 * 60 * 1000; // best-effort; 0 disables timeout
  int64_t ota_drain_timeout_ms = 15 * 1000; // grace window before restart (best-effort)

  // CORS (for browser-based clients). Defaults depend on listen host:
  // - loopback: allow any origin ("*") for local UI dev
  // - non-loopback: disabled unless explicitly configured via --cors-origin
  bool cors_origins_set = false;
  bool cors_disabled = false;
  std::vector<std::string> cors_origins; // values are exact match, or "*"
  std::vector<CorsRouteConfig> cors_routes;
  std::string cors_allow_headers;
  std::string cors_allow_methods;
  bool cors_allow_credentials = false;
  int cors_max_age_seconds = 600;
};

}  // namespace agentd
