#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Builds a bounded, deterministic relationship graph from a memory correlation
// response. The input may contain `entries`, `timeline`, `daily_entries`, and
// `recap_entries`; existing `relationship_graph` fields are ignored.
Json::Value memory_correlation_relationship_graph_from_response(
  const std::string& trace_id,
  const Json::Value& response
);

}  // namespace agentd
