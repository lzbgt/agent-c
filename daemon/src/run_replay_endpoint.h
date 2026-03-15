#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "agent_db.h"

#include <json/json.h>

namespace agentd {

void handle_run_replay_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_run_attestation_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// Internal helper for daemon subsystems that need the same attestation bundle surface
// without going through HTTP.
bool build_run_attestation_bundle_json(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  int64_t run_id,
  Json::Value* out_attestation,
  std::string* out_error
);

}  // namespace agentd
