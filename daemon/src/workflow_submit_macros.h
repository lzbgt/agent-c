#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"

#include <json/json.h>

#include <string>

namespace agentd {

struct DaemonConfig;

// Internal helper used by workflow submit endpoint.
// Expands macro tasks like kind:"edge_parallel" and kind:"delegate_parallel" into
// a deterministic set of concrete tasks.
//
// NOTE: Keep this internal to daemon/src (do not expose in daemon/include) to avoid
// committing to a public API while semantics are still evolving.
bool expand_workflow_submit_macros(
  const DaemonConfig& cfg,
  Json::Value* io_tasks_arr,
  const Json::Value& workflow_defaults,
  AgentDb* db_or_null,
  bool allow_sessions,
  bool allow_inline_api_keys,
  const std::string& session_id,
  const std::string& trace_id,
  HttpResponse* resp
);

}  // namespace agentd
