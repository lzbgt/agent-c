#pragma once

#include <json/json.h>

#include <string>
#include <unordered_map>

namespace agentd {

Json::Value workflow_aggregate_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
);

}  // namespace agentd

