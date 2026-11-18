#pragma once

#include <json/json.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentd {
bool workflow_condition_validate(const Json::Value& spec, std::string* error);
bool workflow_condition_validate_submission(const Json::Value& args, std::string* error);
bool workflow_condition_attach(const Json::Value& task, const std::string& task_id,
    Json::Value* request, Json::Value* dependencies, std::vector<std::string>* dependency_ids,
    std::string* error);
// Returns null when execution should proceed. Otherwise returns a terminal,
// effect-free skip or fail-closed error. Only successful non-skipped sources belong here.
Json::Value workflow_condition_evaluate(const Json::Value& spec,
    const std::unordered_map<std::string, Json::Value>& completed_results, Json::Value* receipt);
void workflow_condition_project(const std::string& request_json, const Json::Value& result,
    Json::Value* task_metadata);
}  // namespace agentd
