#include "workflow_condition.h"
#include "json_util.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace agentd {
namespace {
bool text(const Json::Value& value, size_t max) {
  return value.isString() && value.asString().size() <= max && value.asString().find('\0') == std::string::npos;
}
bool id(const Json::Value& value) {
  if (!text(value, 128) || value.asString().empty()) return false;
  for (const unsigned char c : value.asString())
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.' || c == ':')) return false;
  return true;
}
bool scalar(const Json::Value& value) {
  // Deliberately no numeric coercion, floating-point comparisons or expressions.
  return value.isNull() || value.isBool() || text(value, 1024);
}
bool pointer(const Json::Value& value) {
  if (!text(value, 512) || value.asString().empty() || value.asString()[0] != '/') return false;
  int levels = 0;
  const auto path = value.asString();
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '/' && ++levels > 32) return false;
    if (path[i] == '~' && (++i == path.size() || (path[i] != '0' && path[i] != '1'))) return false;
  }
  return true;
}
const Json::Value* lookup(const Json::Value& root, const std::string& path) {
  const Json::Value* value = &root;
  size_t start = 1;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto end = slash == std::string::npos ? path.size() : slash;
    std::string key;
    for (size_t i = start; i < end; ++i) {
      if (path[i] == '~') key += path[++i] == '0' ? '~' : '/';
      else key += path[i];
    }
    if (value->isObject()) {
      if (!value->isMember(key)) return nullptr;
      value = &(*value)[key];
    } else if (value->isArray()) {
      if (key.empty() || (key.size() > 1 && key[0] == '0')) return nullptr;
      uint64_t index = 0;
      for (const unsigned char c : key) {
        if (c < '0' || c > '9' || index > value->size() / 10) return nullptr;
        index = index * 10 + (c - '0');
        if (index >= value->size()) return nullptr;
      }
      value = &(*value)[static_cast<Json::ArrayIndex>(index)];
    } else return nullptr;
    if (slash == std::string::npos) return value;
    start = slash + 1;
  }
  return nullptr;
}
Json::Value outcome(bool skipped, const std::string& reason) {
  Json::Value out(Json::objectValue);
  out["ok"] = skipped; out["retryable"] = false; out["skipped"] = skipped;
  out["assistant_text"] = skipped ? "Skipped: action condition did not match." : "";
  if (!skipped) out["error"] = reason;
  for (const auto* key : {"elapsed_ms", "tool_calls_total", "steps_executed", "prompt_tokens", "completion_tokens", "total_tokens"})
    out[key] = Json::Int64(0);
  return out;
}
}  // namespace

bool workflow_condition_validate(const Json::Value& spec, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const char* message) { if (error) *error = std::string("invalid when: ") + message; return false; };
  if (!spec.isObject()) return fail("expected task_id, pointer and equals");
  for (const auto& key : spec.getMemberNames())
    if (key != "task_id" && key != "pointer" && key != "equals") return fail("unknown field");
  if (!id(spec["task_id"])) return fail("task_id must be an id-safe string of 1..128 bytes");
  if (!pointer(spec["pointer"])) return fail("pointer must be a valid nonempty JSON pointer, at most 512 bytes and 32 levels");
  if (!spec.isMember("equals") || !scalar(spec["equals"])) return fail("equals must be null, boolean or a string of at most 1024 bytes without NUL");
  return true;
}

bool workflow_condition_validate_submission(const Json::Value& args, std::string* error) {
  const auto fail = [&](const char* message) { if (error) *error = message; return false; };
  if (args["defaults"].isObject() && args["defaults"].isMember("when"))
    return fail("when must be declared on an individual task, not defaults");
  if (!args["tasks"].isArray()) return true; // Ordinary submit validation owns shape errors.
  for (const auto& task : args["tasks"]) {
    if (!task.isObject()) continue;
    const auto kind = task["kind"].isString() ? task["kind"].asString() : std::string();
    if (task.isMember("when") && (kind == "edge_parallel" || kind == "agentd_parallel" || kind == "delegate_parallel"))
      return fail("when is not supported on submit-time expansion macros");
    if (task["request"].isObject() && task["request"].isMember("when"))
      return fail("when must be a top-level task field, not request.when");
    if (task["delegate"].isObject() && task["delegate"]["attempts"].isArray()) {
      for (const auto& attempt : task["delegate"]["attempts"])
        if (attempt.isObject() && (attempt.isMember("when") ||
            (attempt["request"].isObject() && attempt["request"].isMember("when"))))
          return fail("when is not supported inside delegate attempts");
    }
  }
  return true;
}

bool workflow_condition_attach(const Json::Value& task, const std::string& task_id,
    Json::Value* request, Json::Value* dependencies, std::vector<std::string>* dependency_ids,
    std::string* error) {
  if (error) error->clear();
  if (task["request"].isObject() && task["request"].isMember("when")) {
    if (error) *error = "when must be a top-level task field, not request.when";
    return false;
  }
  if (!task.isMember("when")) return true;
  if (!workflow_condition_validate(task["when"], error)) return false;
  const auto source = task["when"]["task_id"].asString();
  if (source == task_id || (task.isMember("allow_error") && task["allow_error"] != false)) {
    if (error) *error = "when cannot reference its own task or permit allow_error";
    return false;
  }
  (*request)["when"] = task["when"];
  if (std::find(dependency_ids->begin(), dependency_ids->end(), source) == dependency_ids->end()) {
    dependency_ids->push_back(source);
    dependencies->append(source);
  }
  return true;
}

Json::Value workflow_condition_evaluate(const Json::Value& spec,
    const std::unordered_map<std::string, Json::Value>& completed_results, Json::Value* receipt) {
  *receipt = Json::Value();
  std::string error;
  if (!workflow_condition_validate(spec, &error)) return outcome(false, error);
  (*receipt)["schema"] = "agentd.workflow.condition.v1";
  (*receipt)["test"] = spec;
  const auto found = completed_results.find(spec["task_id"].asString());
  if (found == completed_results.end()) return outcome(false, "condition source is not a successful non-skipped task");
  const auto* actual = lookup(found->second, spec["pointer"].asString());
  if (!actual) return outcome(false, "condition result pointer is missing");
  if (!scalar(*actual)) return outcome(false, "condition result is not a bounded boolean, string or null");
  // Type mismatches are errors rather than silently taking the opposite branch.
  if (actual->type() != spec["equals"].type()) return outcome(false, "condition value type does not match equals");
  const bool matched = *actual == spec["equals"];
  (*receipt)["actual"] = *actual;
  (*receipt)["matched"] = matched;
  return matched ? Json::Value() : outcome(true, "");
}

void workflow_condition_project(const std::string& request_json, const Json::Value& result, Json::Value* task_metadata) {
  Json::Value request;
  if (!task_metadata || !json_parse_object(request_json, &request, nullptr) || !request.isMember("when") ||
      !workflow_condition_validate(request["when"], nullptr) || !result.isObject()) return;
  const auto& receipt = result["condition"];
  if (!receipt.isObject() || receipt["schema"] != "agentd.workflow.condition.v1" ||
      receipt["test"] != request["when"] || !receipt["matched"].isBool()) return;
  auto& projected = (*task_metadata)["condition"];
  projected["task_id"] = request["when"]["task_id"];
  projected["pointer"] = request["when"]["pointer"];
  projected["matched"] = receipt["matched"];
  if (result["ok"] == true && result["skipped"] == true && receipt["matched"] == false)
    (*task_metadata)["skipped"] = true;
}
}  // namespace agentd
