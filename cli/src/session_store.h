#pragma once

#include "agent/agent.h"

#include <string>

struct SessionStoreConfig {
  std::string root_dir; // e.g. ~/.agent/sessions
};

// File-backed store: <root>/<id>.json
// This is a CLI/host concern (core remains persistence-agnostic).
agent_status_t session_store_load(const SessionStoreConfig& cfg, const std::string& session_id, agent_session_t** out_session);
agent_status_t session_store_save(const SessionStoreConfig& cfg, const std::string& session_id, const agent_session_t* session);

