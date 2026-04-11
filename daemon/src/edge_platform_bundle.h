#pragma once

#include <json/json.h>

#include <cstdint>
#include <map>
#include <string>

namespace agentd {

class AgentDb;

bool enqueue_edge_platform_bundle(
  AgentDb* db_or_null,
  const std::string& target_node_id,
  const std::string& msg_type,
  const char* body_field,
  const Json::Value& bundle,
  const std::map<std::string, std::string>* confidentiality_keys_or_null,
  const std::string& confidential_kid,
  int64_t* out_outbox_id,
  std::string* out_error
);

}  // namespace agentd
