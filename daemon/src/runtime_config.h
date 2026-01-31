#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <string>

namespace agentd {

// Runtime config persistence (daemon-side defaults editable at runtime).
//
// Persisted in the daemon SQLite DB (agentd.db) so all daemon state lives in one place.
// Never expose provider keys via APIs.

// Loads both non-secret defaults and provider keys (best-effort).
bool load_runtime_config_best_effort(AgentDb& db, DaemonConfig* cfg_io, std::string* out_error);

// Saves non-secret defaults (base_url/model/proxy/timeout/summary settings).
bool save_runtime_config_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error);

// Saves provider keys (secrets).
bool save_runtime_secrets_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error);

}  // namespace agentd
