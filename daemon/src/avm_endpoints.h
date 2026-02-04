#pragma once

#include "cors.h"
#include "http_util.h"
#include "runtime_config.h"

namespace agentd {

// AVM integration endpoints (Oren AVM; out-of-process).
//
// These endpoints are intended to let the platform/agent framework leverage AVM's
// "governance surfaces" (job scanning + hashes) without requiring an LLM tool loop.
//
// Security model:
// - requires daemon auth (when enabled)
// - requires yolo_default=true (daemon not in scoped mode)
// - requires explicit env var AGENTD_AVM_BIN to select the avm binary path
//
// Endpoint(s):
// - POST /api/v1/avm/job_scan   (runs: avm --print-job-json <tmp.obc>)

void handle_avm_job_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

