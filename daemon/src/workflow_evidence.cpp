#include "workflow_evidence.h"

#include "workflow_engine_common.h"

#include <algorithm>
#include <cctype>

namespace agentd {

using workflow_engine_internal::json_get_string;

namespace {

std::string workflow_evidence_json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

}  // namespace

std::string workflow_artifact_slug(std::string s) {
  for (char& c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
    c = '_';
  }
  if (s.empty()) return "unknown";
  return s;
}

void persist_workflow_session_artifact_best_effort(
  AgentDb* db,
  int64_t run_id,
  const AgentDb::WorkflowRow& wf,
  const std::string& task_id,
  int64_t ts_unix_ms,
  const Json::Value& artifact
) {
  if (!db || !db->is_open() || run_id <= 0 || !artifact.isObject() || wf.session_id.empty()) return;
  const std::string path = json_get_string(artifact, "path");
  if (path.empty()) return;

  AgentDb::ArtifactRow row;
  row.run_id = run_id;
  row.ts_unix_ms = ts_unix_ms;
  row.session_id = wf.session_id;
  row.tool_call_id = task_id.empty() ? std::string() : std::string("workflow_task:") + task_id;
  row.path = path;
  row.kind = json_get_string(artifact, "kind");
  row.mime = json_get_string(artifact, "mime");
  row.title = json_get_string(artifact, "title");
  row.autoplay = artifact.isMember("autoplay") && artifact["autoplay"].isBool() && artifact["autoplay"].asBool();
  row.repeat = artifact.isMember("repeat") && artifact["repeat"].isInt() ? std::max(1, artifact["repeat"].asInt()) : 1;
  row.artifact_json = workflow_evidence_json_stringify_compact(artifact);
  (void)db->insert_artifact(row, nullptr, nullptr);
}

int64_t create_workflow_evidence_run_best_effort(
  AgentDb* db,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  int64_t ts_unix_ms,
  const Json::Value& result
) {
  if (!db || !db->is_open() || wf.session_id.empty()) return 0;
  AgentDb::RunRow row;
  row.session_id = wf.session_id;
  row.job_id = wf.workflow_id.empty() ? task.task_id : (wf.workflow_id + ":" + task.task_id + ":evidence");
  row.ts_unix_ms = ts_unix_ms;
  row.prompt = std::string("[workflow_evidence] ") + task.task_id;
  row.tools = "workflow";
  row.model = "workflow_evidence";
  row.base_url = "/api/v1/workflow";
  row.stream_assistant = false;
  Json::Value request(Json::objectValue);
  request["schema"] = "agentd.workflow_evidence_request.v1";
  request["workflow_id"] = wf.workflow_id;
  request["task_id"] = task.task_id;
  request["kind"] = "workflow_evidence";
  row.request_json = workflow_evidence_json_stringify_compact(request);
  row.ok = result.isObject() && result.isMember("ok") && result["ok"].isBool() && result["ok"].asBool();
  row.stop_reason = row.ok ? "done" : "error";
  row.response_json = workflow_evidence_json_stringify_compact(result);
  if (result.isObject() && result.isMember("error") && result["error"].isString()) {
    row.error = result["error"].asString();
    row.last_error_reason = row.error;
  }
  int64_t run_id = 0;
  if (!db->insert_run(row, &run_id, nullptr) || run_id <= 0) return 0;
  return run_id;
}

}  // namespace agentd
