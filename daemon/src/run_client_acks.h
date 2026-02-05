#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace agentd {

struct ExpectedClientAck {
  // category: "artifact" | "client_rpc" | "client_probe" | "ui_action"
  std::string category;
  std::string tool_call_id;
  std::string rpc_id;
  std::string rpc_kind;
};

std::vector<ExpectedClientAck> collect_expected_client_acks(const Json::Value& events_out);

Json::Value verify_expected_client_acks(
  AgentDb& db,
  const std::string& session_id,
  int64_t after_unix_ms,
  const std::vector<ExpectedClientAck>& expected,
  int timeout_ms
);

}  // namespace agentd

