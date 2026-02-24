#pragma once

#include "agentd/http_types.h"

namespace agentd {

struct DaemonConfig;
struct CorsConfig;
class AgentDb;

void handle_approvals_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_approvals_prefix_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
