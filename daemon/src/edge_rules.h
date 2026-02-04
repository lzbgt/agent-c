#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <string>

namespace agentd {

// Apply enabled rules for an inbound SENSOR_EVENT (best-effort).
void edge_rules_apply_for_sensor_event_best_effort(
  AgentDb* db,
  const std::string& sensor_node_id,
  const std::string& sensor_msg_id,
  const std::string& event_type,
  int64_t event_ts_utc_ms,
  double confidence,
  const Json::Value& data
);

}  // namespace agentd

