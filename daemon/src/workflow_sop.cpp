#include "workflow_sop.h"

#include "agent/json_c14n.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace agentd {
namespace {

bool safe_id(const Json::Value& v) {
  if (!v.isString() || v.asString().empty() || v.asString().size() > 128) return false;
  for (const unsigned char c : v.asString()) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':')) return false;
  }
  return true;
}

bool fields_allowed(const Json::Value& v, const std::unordered_set<std::string>& allowed) {
  for (const auto& key : v.getMemberNames()) if (!allowed.count(key)) return false;
  return true;
}

}  // namespace

bool workflow_compile_sop(Json::Value* args, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) {
    if (error) *error = "invalid sop: " + message;
    return false;
  };
  if (!args || !args->isObject()) return fail("expected workflow object");
  if (args->isMember("sop_compiled")) return fail("sop_compiled is server-owned");
  if (!args->isMember("sop")) return true;
  if (args->isMember("tasks")) return fail("supply either sop or tasks");
  if (args->isMember("infer_depends_on") &&
      (!(*args)["infer_depends_on"].isBool() || (*args)["infer_depends_on"].asBool())) {
    return fail("dependency inference is not supported; stage order defines dependencies");
  }
  const auto& sop = (*args)["sop"];
  if (!sop.isObject() || !fields_allowed(sop, {"schema", "id", "version", "title", "stages"})) {
    return fail("expected object with schema, id, version, optional title, and stages");
  }
  if (sop["schema"] != "agentd.sop.v1") return fail("unsupported schema (expected agentd.sop.v1)");
  if (!safe_id(sop["id"]) || !safe_id(sop["version"])) return fail("id and version must be id-safe strings (1..128 bytes)");
  if (sop.isMember("title") && (!sop["title"].isString() || sop["title"].asString().size() > 256)) return fail("title must be a string of at most 256 bytes");
  const auto& stages = sop["stages"];
  if (!stages.isArray() || stages.empty() || stages.size() > 32) return fail("expected 1..32 stages");

  Json::Value tasks(Json::arrayValue), manifest(Json::objectValue);
  manifest["schema"] = "agentd.sop.compiled.v1";
  manifest["id"] = sop["id"];
  manifest["version"] = sop["version"];
  manifest["stages"] = Json::Value(Json::arrayValue);
  std::unordered_set<std::string> stage_ids, task_ids;
  std::vector<std::string> previous;
  // Submit-time macros are excluded: their derived IDs would change stage boundaries.
  const std::unordered_set<std::string> kinds = {
    "llm", "delay", "decision", "host_tool", "http_json", "agentd_call", "aggregate", "avm_capsule",
    "memory_put", "memory_consolidate", "memory_search", "memory_timeline",
    "memory_structured_query", "memory_correlate", "memory_query", "experience_record",
    "edge_invoke", "edge_wait_sensor", "delegate"
  };
  for (const auto& stage : stages) {
    if (!stage.isObject() || !fields_allowed(stage, {"id", "title", "mode", "actions"}) || !safe_id(stage["id"])) return fail("stage requires an id and actions; unknown stage fields are rejected");
    const std::string stage_id = stage["id"].asString();
    if (!stage_ids.insert(stage_id).second) return fail("duplicate stage id: " + stage_id);
    if (stage.isMember("title") && (!stage["title"].isString() || stage["title"].asString().size() > 256)) return fail("invalid stage title: " + stage_id);
    if (stage.isMember("mode") && !stage["mode"].isString()) return fail("stage mode must be sequence or parallel");
    const std::string mode = stage.get("mode", "sequence").asString();
    if (mode != "sequence" && mode != "parallel") return fail("stage mode must be sequence or parallel");
    const auto& actions = stage["actions"];
    if (!actions.isArray() || actions.empty()) return fail("stage actions must be non-empty: " + stage_id);
    Json::Value stage_manifest(Json::objectValue);
    stage_manifest["id"] = stage_id;
    stage_manifest["mode"] = mode;
    if (stage.isMember("title")) stage_manifest["title"] = stage["title"];
    stage_manifest["task_ids"] = Json::Value(Json::arrayValue);
    std::vector<std::string> current;
    for (const auto& action : actions) {
      if (tasks.size() >= 128) return fail("too many actions (max 128)");
      if (!action.isObject() || !safe_id(action["task_id"])) return fail("actions require globally unique task_id strings");
      const std::string id = action["task_id"].asString();
      if (!task_ids.insert(id).second) return fail("duplicate action task_id: " + id);
      if (action.isMember("depends_on") || action.isMember("id")) return fail("action dependencies are generated from stages: " + id);
      if (!fields_allowed(action, {"task_id", "kind", "request", "allow_error", "priority", "max_attempts", "expect", "inputs", "delay_ms", "result", "decision", "host_tool", "http_json", "agentd_call", "aggregate", "capsule", "memory_put", "memory_consolidate", "memory_search", "memory_timeline", "memory_structured_query", "memory_correlate", "memory_query", "experience_record", "edge", "edge_wait_sensor", "delegate"})) return fail("unsupported action field: " + id);
      if (action.isMember("allow_error") && (!action["allow_error"].isBool() || action["allow_error"].asBool())) return fail("allow_error is unsupported in SOP v1: " + id);
      if (action.isMember("kind") && (!action["kind"].isString() || !kinds.count(action["kind"].asString()))) return fail("unsupported action kind: " + id);
      Json::Value task = action;
      task["depends_on"] = Json::Value(Json::arrayValue);
      if (mode == "sequence" && !current.empty()) {
        task["depends_on"].append(current.back());
      } else {
        for (const auto& dep : previous) task["depends_on"].append(dep);
      }
      tasks.append(task);
      current.push_back(id);
      stage_manifest["task_ids"].append(id);
    }
    previous = mode == "sequence" ? std::vector<std::string>{current.back()} : current;
    manifest["stages"].append(stage_manifest);
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string source = Json::writeString(writer, sop);
  char digest[80] = {}, detail[160] = {};
  if (agent_json_c14n_sha256_token(source.data(), source.size(), digest, detail, sizeof(detail)) != AGENT_OK) return fail("cannot hash definition");
  manifest["definition_hash"] = digest;
  (*args)["tasks"] = std::move(tasks);
  (*args)["sop_compiled"] = std::move(manifest);
  return true;
}

}  // namespace agentd
