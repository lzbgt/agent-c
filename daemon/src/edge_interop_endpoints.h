#pragma once

#include "agent_db.h"
#include "cors.h"
#include "http_util.h"
#include "net_compat.h"
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
// - POST /api/v1/edge/node/consensus_runtime
// - GET /api/v1/edge/node/consensus_runtime?node_id=...
// - GET /api/v1/edge/node/caps?node_id=...
// - GET /api/v1/edge/node/manifest_bundle?node_id=...
// - POST /api/v1/edge/node/manifest_bundle/send
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

void handle_edge_node_manifest_bundle_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_node_manifest_bundle_send_endpoint(
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

// Rules (automation): SENSOR_EVENT -> TASK_ASSIGN.
// - POST   /api/v1/edge/rule/upsert
// - GET    /api/v1/edge/rules
// - DELETE /api/v1/edge/rule?rule_id=...
void handle_edge_rule_upsert_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_rules_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_rule_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// Durable edge workflows (UM‑WF; executed via edge task dispatch).
// - POST /api/v1/edge/workflow/submit
// - GET  /api/v1/edge/workflow?workflow_id=...&include_steps=1
// - GET  /api/v1/edge/workflows?status=QUEUED|RUNNING|SUCCEEDED|FAILED|CANCELED&limit=...
// - POST /api/v1/edge/workflow/cancel
// - GET  /api/v1/edge/workflow/events?workflow_id=...&cursor=0&limit=256
// - GET  /api/v1/edge/workflow/stream?workflow_id=...&cursor=0  (SSE; ends with edge_workflow_done)
void handle_edge_workflow_submit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_workflow_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_workflow_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_workflow_cancel_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_workflow_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_workflow_stream_endpoint(
  const std::string& daemon_auth_token,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  socket_t client_fd
);

}  // namespace agentd
