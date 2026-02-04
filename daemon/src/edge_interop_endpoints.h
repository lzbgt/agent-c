#pragma once

#include "agent_db.h"
#include "cors.h"
#include "http_util.h"
#include "runtime_config.h"

namespace agentd {

// UM‑EAIS / UM‑BMP transport endpoints (HTTP mapping).
//
// These endpoints implement a minimal transport for UM‑BMP envelopes:
// - platform ingest: POST /api/v1/edge/message (any UM‑BMP message)
// - node poll:       GET  /api/v1/edge/outbox?node_id=...&cursor=0&limit=256
//
// Plus debug helpers:
// - GET /api/v1/edge/nodes
// - GET /api/v1/edge/node?node_id=...
// - GET /api/v1/edge/node/caps?node_id=...
// - POST /api/v1/edge/task/assign  (platform helper: enqueue TASK_ASSIGN)
// - GET /api/v1/edge/task?task_id=...&step_id=...

void handle_edge_message_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_outbox_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_nodes_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_node_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_node_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_task_assign_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_task_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

