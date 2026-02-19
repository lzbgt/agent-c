#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "daemon_config.h"

#include <json/json.h>

namespace agentd {

void workflow_submit_handle(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  Json::Value args,
  HttpResponse* resp
);

}  // namespace agentd
