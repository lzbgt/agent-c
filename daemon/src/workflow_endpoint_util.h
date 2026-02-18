#pragma once

#include <json/json.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentd {

bool parse_citation_path_line(const std::string& citation, std::string* out_path, int* out_line);
bool id_is_safe(const std::string& s);
bool is_safe_relpath_md(const std::string& p);
std::string new_workflow_id();
std::string redact_json_best_effort(const std::string& json);
bool validate_dag_or_error(
  const std::vector<std::string>& task_ids,
  const std::unordered_map<std::string, std::vector<std::string>>& deps,
  std::string* out_err
);
void collect_referenced_task_ids_from_json_value(
  const Json::Value& v,
  std::unordered_set<std::string>* out
);

}  // namespace agentd
