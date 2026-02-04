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
// - POST /api/v1/avm/policy_scan (runs: avm --print-policy-json <tmp.obc>)
// - POST /api/v1/avm/inspect     (runs: avm --inspect-json <tmp.obc>)
// - POST /api/v1/avm/verify_strict (runs: avm --verify-strict <tmp.obc>)
// - POST /api/v1/avm/trace_hash  (runs: avm --print-trace-hash <tmp.obc>)

void handle_avm_job_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_avm_policy_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_avm_inspect_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_avm_verify_strict_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_avm_trace_hash_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
