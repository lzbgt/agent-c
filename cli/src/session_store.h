#pragma once

#include "agent/agent.h"

#include <cstddef>
#include <string>
#include <vector>

struct SessionStoreConfig {
  std::string root_dir; // e.g. ~/.agent/sessions

  // Bounded per-session audit log retention:
  // - File: <root>/<id>.events.jsonl
  // - Backups: <root>/<id>.events.jsonl.1, .2, ...
  //
  // This audit log can grow quickly during tool loops (full prompts/tool calls/results),
  // so the default is intentionally larger than `client_events_*`.
  size_t audit_max_bytes = 16 * 1024 * 1024;
  size_t audit_max_files = 3;
  size_t audit_max_record_bytes = 1024 * 1024;

  // Bounded UI client event log retention (prevents unbounded growth on long-running daemons).
  //
  // File: <root>/<id>.client_events.jsonl
  // Backups: <root>/<id>.client_events.jsonl.1, .2, ...
  //
  // Semantics:
  // - `client_events_max_bytes == 0` disables rotation (not recommended).
  // - `client_events_max_files == 0` means "keep only the active file" (truncate/overwrite when rotating).
  // - Rotation is best-effort and process-local (intended for a single daemon instance per sessions_root_dir).
  size_t client_events_max_bytes = 2 * 1024 * 1024;
  size_t client_events_max_files = 3;
  size_t client_events_max_event_bytes = 256 * 1024;
};

// File-backed store:
// - Primary (portable, JSON-free): <root>/<id>.sess
// - Optional (when built with JSONCPP): <root>/<id>.json
// This is a CLI/host concern (core remains persistence-agnostic).
agent_status_t session_store_load(const SessionStoreConfig& cfg, const std::string& session_id, agent_session_t** out_session);
agent_status_t session_store_save(const SessionStoreConfig& cfg, const std::string& session_id, const agent_session_t* session);

// Lists known session ids by scanning <root>/*.sess and <root>/*.json (excluding *.events.jsonl).
agent_status_t session_store_list(const SessionStoreConfig& cfg, std::vector<std::string>* out_session_ids);

// Deletes <root>/<id>.sess, <root>/<id>.json, and <root>/<id>.events.jsonl if present.
agent_status_t session_store_delete(const SessionStoreConfig& cfg, const std::string& session_id);

// Appends a per-run audit record to <root>/<id>.events.jsonl.
// `record_json` should be a single JSON object string.
agent_status_t session_store_append_audit_jsonl(const SessionStoreConfig& cfg, const std::string& session_id, const std::string& record_json);

// Reads up to `max_bytes` from the end of <root>/<id>.events.jsonl (best-effort).
agent_status_t session_store_read_audit_tail(const SessionStoreConfig& cfg, const std::string& session_id, size_t max_bytes, std::string* out_text);

// Reads up to `max_bytes` of the newest audit entries, optionally including rotated backups
// (<id>.events.jsonl.1, .2, ...) if the current file does not fill the budget.
agent_status_t session_store_read_audit_tail_multi(
  const SessionStoreConfig& cfg,
  const std::string& session_id,
  size_t max_bytes,
  size_t max_files,
  std::string* out_text
);

// UI client events (bidirectional UI→daemon protocol):
// - Each line is a single JSON object string.
// - Used by `POST /api/v1/session/ui_event` and the `ui_wait_event` host tool.
//
// File path: <root>/<id>.client_events.jsonl
agent_status_t session_store_append_client_event_jsonl(const SessionStoreConfig& cfg, const std::string& session_id, const std::string& event_json);

// Reads up to `max_bytes` from the end of <root>/<id>.client_events.jsonl (best-effort).
agent_status_t session_store_read_client_event_tail(const SessionStoreConfig& cfg, const std::string& session_id, size_t max_bytes, std::string* out_text);

// Reads up to `max_bytes` of the newest client events, optionally including rotated backups
// (<id>.client_events.jsonl.1, .2, ...) if the current file does not fill the budget.
//
// Returned text is JSONL (one JSON object per line), best-effort parseable.
agent_status_t session_store_read_client_event_tail_multi(
  const SessionStoreConfig& cfg,
  const std::string& session_id,
  size_t max_bytes,
  size_t max_files,
  std::string* out_text
);
