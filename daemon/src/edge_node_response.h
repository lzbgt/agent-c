#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <cstddef>
#include <string>
#include <unordered_set>

namespace agentd {

struct DaemonConfig;

Json::Value build_edge_node_summary_json(
  const AgentDb::EdgeNodeRow* row_or_null,
  const Json::Value& runtime_or_null,
  const std::string& fallback_node_id
);

void append_runtime_only_edge_node_summaries(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  size_t limit,
  std::unordered_set<std::string>* seen_node_ids,
  Json::Value* arr
);

void append_edge_node_detail_json(
  const DaemonConfig& cfg,
  const AgentDb::EdgeNodeRow* row_or_null,
  Json::Value* out
);

}  // namespace agentd
