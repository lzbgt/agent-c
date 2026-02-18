#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <string>

namespace agentd {

// Runtime config persistence (daemon-side defaults editable at runtime).
//
// Persisted in the daemon SQLite DB (agentd.db) so all daemon state lives in one place.
// Never expose provider keys via APIs.

struct RuntimeConfigLoadOptions {
  // When false, the loader will not override existing cfg fields (e.g. when CLI flags were used).
  bool override_workflow_http_allow_hosts = true;
  bool override_workflow_http_allow_cidrs = true;
  bool override_workflow_http_deny_cidrs = true;
  bool override_workflow_http_deny_private_addrs = true;
  bool override_workflow_http_dns_pin = true;
  bool override_upload_max_bytes = true;
  bool override_blob_store = true;
};

// Loads both non-secret defaults and provider keys (best-effort).
bool load_runtime_config_best_effort(AgentDb& db, DaemonConfig* cfg_io, std::string* out_error);
bool load_runtime_config_best_effort(AgentDb& db, DaemonConfig* cfg_io, std::string* out_error, const RuntimeConfigLoadOptions& opt);

// Saves non-secret defaults (base_url/model/proxy/timeout/summary + tool-loop defaults).
bool save_runtime_config_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error);

// Saves provider keys (secrets).
bool save_runtime_secrets_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error);

}  // namespace agentd
