#pragma once

#include "agent_db.h"
#include "cors.h"
#include "agentd/http_types.h"
#include "net_compat.h"

namespace agentd {

// GET /api/v1/workflow/stream?workflow_id=...&cursor=<event_id>
//
// Streams durable workflow events from SQLite as Server-Sent Events.
// - event: workflow_event (per workflow_events row)
// - event: workflow_done (terminal summary; then stream closes)
void handle_workflow_stream_endpoint(
  const std::string& daemon_auth_token,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  socket_t client_fd
);

}  // namespace agentd
