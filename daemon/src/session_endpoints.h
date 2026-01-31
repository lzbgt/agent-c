#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "agent_db.h"

#include <string>

namespace agentd {

void handle_sessions_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_audit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_artifacts_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_new_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_ui_event_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_client_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

// Lists distinct clients observed in the session-scoped client event log.
void handle_session_clients_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
