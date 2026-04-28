#include "workflow_experience.h"

#include "string_util.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace agentd {
namespace workflow_engine_internal {
namespace {

std::mutex& experience_file_mutex() {
  static std::mutex mu;
  return mu;
}

bool safe_label(const std::string& s) {
  if (s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':' || c == '/';
    if (!ok) return false;
  }
  return true;
}

std::string json_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

Json::Value bounded_result_surface(const Json::Value& v, int64_t max_chars) {
  Json::Value out(Json::objectValue);
  const std::string s = json_compact(v);
  out["truncated"] = (bool)(max_chars > 0 && (int64_t)s.size() > max_chars);
  if (out["truncated"].asBool()) {
    out["result_json_prefix"] = s.substr(0, (size_t)max_chars);
    out["result_json_chars"] = (Json::Int64)s.size();
  } else {
    out["result"] = v;
  }
  const bool ok = v.isObject() && v.isMember("ok") && v["ok"].isBool() && v["ok"].asBool();
  out["ok"] = ok;
  if (v.isObject() && v.isMember("kind") && v["kind"].isString()) out["kind"] = v["kind"].asString();
  if (v.isObject() && v.isMember("error") && v["error"].isString()) out["error"] = v["error"].asString();
  return out;
}

}  // namespace

Json::Value workflow_experience_record_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  int64_t now_unix_ms
) {
  Json::Value out(Json::objectValue);
  out["kind"] = "experience_record";
  out["ok"] = false;
  out["assistant_text"] = "";

  const Json::Value er =
    rr.isMember("experience_record") && rr["experience_record"].isObject()
    ? rr["experience_record"]
    : Json::Value(Json::objectValue);

  const std::string label =
    er.isMember("label") && er["label"].isString() ? trim_copy(er["label"].asString()) : "";
  if (!safe_label(label)) {
    out["error"] = "experience_record.label must be label-safe and <= 128 chars";
    return out;
  }

  int64_t max_result_chars = 8192;
  if (er.isMember("max_result_chars")) {
    if (!(er["max_result_chars"].isInt64() || er["max_result_chars"].isUInt64() || er["max_result_chars"].isInt())) {
      out["error"] = "experience_record.max_result_chars must be an integer";
      return out;
    }
    max_result_chars = er["max_result_chars"].asInt64();
  }
  if (max_result_chars < 1024) max_result_chars = 1024;
  if (max_result_chars > 65536) max_result_chars = 65536;

  std::vector<std::string> task_ids;
  if (er.isMember("task_ids")) {
    if (!er["task_ids"].isArray()) {
      out["error"] = "experience_record.task_ids must be an array of strings";
      return out;
    }
    for (Json::ArrayIndex i = 0; i < er["task_ids"].size(); i++) {
      if (!er["task_ids"][i].isString()) {
        out["error"] = "experience_record.task_ids entries must be strings";
        return out;
      }
      const std::string id = trim_copy(er["task_ids"][i].asString());
      if (id.empty()) continue;
      task_ids.push_back(id);
    }
  } else {
    task_ids.reserve(result_json_by_task.size());
    for (const auto& kv : result_json_by_task) {
      if (!kv.first.empty() && kv.first != task.task_id) task_ids.push_back(kv.first);
    }
    std::sort(task_ids.begin(), task_ids.end());
  }

  if (task_ids.empty()) {
    out["error"] = "experience_record has no source task ids";
    return out;
  }

  Json::Value results(Json::objectValue);
  int included = 0;
  int ok_count = 0;
  Json::Value missing(Json::arrayValue);
  for (const auto& id : task_ids) {
    auto it = result_json_by_task.find(id);
    if (it == result_json_by_task.end()) {
      missing.append(id);
      continue;
    }
    Json::Value bounded = bounded_result_surface(it->second, max_result_chars);
    if (bounded.isMember("ok") && bounded["ok"].isBool() && bounded["ok"].asBool()) ok_count++;
    results[id] = bounded;
    included++;
  }

  if (included == 0) {
    out["error"] = "experience_record source tasks are not completed yet";
    out["missing_task_ids"] = missing;
    return out;
  }

  double reward = included > 0 ? (double)ok_count / (double)included : 0.0;
  if (er.isMember("reward")) {
    if (!er["reward"].isNumeric()) {
      out["error"] = "experience_record.reward must be numeric";
      return out;
    }
    reward = er["reward"].asDouble();
  }
  if (reward < -1.0) reward = -1.0;
  if (reward > 1.0) reward = 1.0;

  Json::Value record(Json::objectValue);
  record["schema"] = "agentd_experience_record_v1";
  record["workflow_id"] = wf.workflow_id;
  record["task_id"] = task.task_id;
  record["session_id"] = wf.session_id;
  record["trace_id"] = wf.trace_id;
  record["ts_unix_ms"] = (Json::Int64)now_unix_ms;
  if (!label.empty()) record["label"] = label;
  record["reward"] = reward;
  record["source_task_count"] = included;
  record["source_task_ok_count"] = ok_count;
  record["source_results_by_task"] = results;
  if (!missing.empty()) record["missing_task_ids"] = missing;
  if (er.isMember("metadata") && er["metadata"].isObject()) record["metadata"] = er["metadata"];

  std::filesystem::path base = cfg.state_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(cfg.state_dir);
  std::filesystem::path rel = std::filesystem::path("rl") / "experience_records.jsonl";
  std::filesystem::path path = base / rel;

  try {
    std::filesystem::create_directories(path.parent_path());
    const std::string line = json_compact(record);
    {
      std::lock_guard<std::mutex> lock(experience_file_mutex());
      std::ofstream f(path, std::ios::out | std::ios::app);
      if (!f) {
        out["error"] = "failed to open experience record file";
        out["path"] = rel.generic_string();
        return out;
      }
      f << line << "\n";
      if (!f) {
        out["error"] = "failed to write experience record file";
        out["path"] = rel.generic_string();
        return out;
      }
    }
  } catch (const std::exception& e) {
    out["error"] = std::string("failed to persist experience record: ") + e.what();
    out["path"] = rel.generic_string();
    return out;
  }

  Json::Value ev(Json::objectValue);
  ev["workflow_id"] = wf.workflow_id;
  ev["task_id"] = task.task_id;
  ev["schema"] = record["schema"];
  ev["path"] = rel.generic_string();
  ev["reward"] = reward;
  ev["source_task_count"] = included;
  ev["source_task_ok_count"] = ok_count;
  ev["ts_unix_ms"] = (Json::Int64)now_unix_ms;
  insert_workflow_event_best_effort(db, wf.workflow_id, task.task_id, "experience_record", now_unix_ms, ev);

  out["ok"] = true;
  out["schema"] = record["schema"];
  out["path"] = rel.generic_string();
  out["reward"] = reward;
  out["source_task_count"] = included;
  out["source_task_ok_count"] = ok_count;
  if (!missing.empty()) out["missing_task_ids"] = missing;
  out["assistant_text"] = std::string("experience_record: ") + rel.generic_string();
  return out;
}

}  // namespace workflow_engine_internal
}  // namespace agentd
