#pragma once

#include "agent/agent.h"

#include <string>
#include <vector>

struct SessionStoreConfig {
  std::string root_dir; // e.g. ~/.agent/sessions
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

// UI client events (bidirectional UI→daemon protocol):
// - Each line is a single JSON object string.
// - Used by `POST /api/v1/session/ui_event` and the `ui_wait_event` host tool.
//
// File path: <root>/<id>.client_events.jsonl
agent_status_t session_store_append_client_event_jsonl(const SessionStoreConfig& cfg, const std::string& session_id, const std::string& event_json);

// Reads up to `max_bytes` from the end of <root>/<id>.client_events.jsonl (best-effort).
agent_status_t session_store_read_client_event_tail(const SessionStoreConfig& cfg, const std::string& session_id, size_t max_bytes, std::string* out_text);
