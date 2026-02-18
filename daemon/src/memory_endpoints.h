#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "http_server.h"
#include "openai_client.h"

namespace agentd {

// POST /api/v1/memory/consolidate
void handle_memory_consolidate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_memory_retention_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/memory/checkpoints
// Lists structured memory checkpoint snapshots under state_dir/memory/checkpoints.
void handle_memory_checkpoints_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/memory/correlate?trace_id=...
// Bounded correlation query: selects (by default) the newest structured checkpoint in a time window
// and returns structured memory keys whose evidence sources mention `trace:<trace_id>`.
void handle_memory_correlate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/memory/query
// Bounded structured-memory query: selects the newest structured checkpoint in a time window and returns
// current-view keys filtered by optional `key_prefix`.
void handle_memory_query_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/memory/index
// Lightweight index of memory files (paths + size/line/token estimates).
void handle_memory_index_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_memory_salience_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET/POST /api/v1/memory/recaps
// - GET: list stored recaps
// - POST: generate a new recap (LLM-backed, bounded)
void handle_memory_recaps_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
