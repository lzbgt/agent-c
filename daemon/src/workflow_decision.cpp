#include "workflow_decision.h"
#include "workflow_endpoints.h"
#include "agent/json_c14n.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <chrono>
#include <unordered_set>

namespace agentd {
namespace {
int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer, value);
}
std::string hash(const Json::Value& value) {
  const auto input = encode(value);
  char digest[80] = {}, error[160] = {};
  if (agent_json_c14n_sha256_token(input.data(), input.size(), digest, error, sizeof(error)) != AGENT_OK) return {};
  return digest;
}
bool bounded_text(const Json::Value& value, size_t min, size_t max) {
  return value.isString() && value.asString().size() >= min && value.asString().size() <= max &&
    value.asString().find('\0') == std::string::npos;
}
bool safe_id(const Json::Value& value, size_t max) {
  if (!bounded_text(value, 1, max)) return false;
  for (const unsigned char c : value.asString())
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == ':')) return false;
  return true;
}
bool fields(const Json::Value& value, const std::unordered_set<std::string>& allowed) {
  for (const auto& key : value.getMemberNames()) if (!allowed.count(key)) return false;
  return true;
}
Json::Value normalized(Json::Value spec) {
  spec["timeout_ms"] = spec.get("timeout_ms", Json::Int64(0));
  return spec;
}
AgentDb::WorkflowDecisionRow definition(const std::string& workflow, const std::string& task, const Json::Value& spec) {
  AgentDb::WorkflowDecisionRow row;
  row.workflow_id = workflow; row.task_id = task;
  const auto value = normalized(spec);
  row.spec_json = encode(value); row.request_hash = hash(value);
  Json::Value identity(Json::arrayValue); identity.append(workflow); identity.append(task); identity.append(row.request_hash);
  row.decision_id = "wd_" + hash(identity);
  return row;
}
bool verified(const AgentDb::WorkflowDecisionRow& row, Json::Value* spec) {
  std::string error;
  if (!json_parse_object(row.spec_json, spec, &error) || !workflow_decision_validate(*spec, &error)) return false;
  const auto expected = definition(row.workflow_id, row.task_id, *spec);
  if (expected.request_hash.empty() || row.request_hash != expected.request_hash || row.decision_id != expected.decision_id) return false;
  if (row.state == "resolved") {
    for (const auto& choice : (*spec)["choices"])
      if (choice["id"] == row.choice_id && choice["outcome"] == row.outcome) return true;
    return false;
  }
  return row.state == "waiting" || row.state == "expired";
}
Json::Value metadata(const AgentDb::WorkflowDecisionRow& row, const Json::Value& spec) {
  Json::Value out(Json::objectValue);
  out["workflow_id"] = row.workflow_id; out["task_id"] = row.task_id;
  out["decision_id"] = row.decision_id; out["request_hash"] = row.request_hash;
  out["prompt"] = spec["prompt"]; out["choices"] = spec["choices"]; out["state"] = row.state;
  out["available"] = false;
  out["created_unix_ms"] = Json::Int64(row.created_unix_ms);
  if (row.expires_unix_ms > 0) out["expires_unix_ms"] = Json::Int64(row.expires_unix_ms);
  if (row.resolved_unix_ms > 0) out["resolved_unix_ms"] = Json::Int64(row.resolved_unix_ms);
  if (row.state == "resolved") {
    out["choice_id"] = row.choice_id; out["outcome"] = row.outcome; out["note"] = row.note;
  }
  return out;
}
Json::Value failure(const std::string& message) {
  Json::Value out(Json::objectValue);
  out["ok"] = false; out["kind"] = "decision"; out["retryable"] = false;
  out["assistant_text"] = ""; out["error"] = message;
  out["elapsed_ms"] = Json::Int64(0); out["tool_calls_total"] = Json::Int64(0);
  out["steps_executed"] = Json::Int64(0);
  out["usage"]["prompt_tokens"] = Json::Int64(0); out["usage"]["completion_tokens"] = Json::Int64(0);
  out["usage"]["total_tokens"] = Json::Int64(0);
  return out;
}
}

bool workflow_decision_validate(const Json::Value& spec, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) { if (error) *error = "invalid decision: " + message; return false; };
  if (!spec.isObject() || !fields(spec, {"prompt", "choices", "timeout_ms"})) return fail("expected prompt, choices and optional timeout_ms");
  if (!bounded_text(spec["prompt"], 1, 2048)) return fail("prompt must be 1..2048 bytes without NUL");
  if (spec.isMember("timeout_ms") && (!spec["timeout_ms"].isInt64() || spec["timeout_ms"].asInt64() < 0 ||
      spec["timeout_ms"].asInt64() > 2592000000LL)) return fail("timeout_ms must be an integer from 0 to 2592000000");
  const auto& choices = spec["choices"];
  if (!choices.isArray() || choices.empty() || choices.size() > 8) return fail("expected 1..8 choices");
  std::unordered_set<std::string> ids;
  for (const auto& choice : choices) {
    if (!choice.isObject() || !fields(choice, {"id", "label", "outcome"}) || !safe_id(choice["id"], 64) ||
        !bounded_text(choice["label"], 1, 128) || (choice["outcome"] != "continue" && choice["outcome"] != "fail"))
      return fail("choices require an id, label and continue/fail outcome");
    if (!ids.insert(choice["id"].asString()).second) return fail("choice ids must be unique");
  }
  return true;
}

Json::Value workflow_decision_step(AgentDb* db, const AgentDb::WorkflowTaskRow& task,
    const Json::Value& spec, bool* yielded) {
  if (yielded) *yielded = false;
  std::string error;
  if (!db || !yielded || !workflow_decision_validate(spec, &error)) return failure(error.empty() ? "decision storage unavailable" : error);
  const auto def = definition(task.workflow_id, task.task_id, spec);
  AgentDb::WorkflowDecisionRow row;
  if (def.request_hash.empty() || !db->prepare_workflow_decision(task, def, normalized(spec)["timeout_ms"].asInt64(), &row, yielded, &error))
    return failure("cannot persist process decision");
  if (*yielded) return Json::Value();
  if (row.state == "stopped") { auto out = failure("workflow stopped"); out["cancelled"] = true; return out; }
  Json::Value saved;
  if (!verified(row, &saved)) return failure("stored decision integrity check failed");
  auto out = failure(row.state == "expired" ? "decision expired" : "decision selected a fail outcome");
  out["decision"] = metadata(row, saved);
  if (row.state == "resolved") {
    out["ok"] = row.outcome == "continue"; out["choice_id"] = row.choice_id;
    out["outcome"] = row.outcome; out["note"] = row.note;
    for (const auto& choice : saved["choices"]) if (choice["id"] == row.choice_id) out["assistant_text"] = choice["label"];
    if (out["ok"].asBool()) out.removeMember("error");
  }
  return out;
}

bool workflow_decisions_projection(AgentDb* db, const AgentDb::WorkflowRow& workflow,
    Json::Value* out, std::string* error) {
  if (!db || !out) return false;
  *out = Json::Value(Json::arrayValue);
  std::vector<AgentDb::WorkflowDecisionRow> rows;
  if (!db->list_workflow_decisions(workflow.workflow_id, &rows, error)) return false;
  if (rows.empty()) return true;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  if (!db->list_workflow_tasks(workflow.workflow_id, &tasks, error)) return false;
  const auto now = now_ms();
  const bool active = !workflow.cancel_requested && (workflow.status == "queued" || workflow.status == "running") &&
    !(workflow.deadline_unix_ms > 0 && workflow.deadline_unix_ms <= now);
  for (const auto& row : rows) {
    Json::Value spec;
    if (!verified(row, &spec)) { if (error) *error = "stored decision integrity check failed"; return false; }
    auto value = metadata(row, spec);
    if (row.state == "waiting") {
      std::string status;
      for (const auto& task : tasks) if (task.task_id == row.task_id) status = task.status;
      if (!active || (status != "queued" && status != "running")) value["state"] = "stopped";
      else if (row.expires_unix_ms > 0 && row.expires_unix_ms <= now) value["state"] = "expired";
      else value["available"] = status == "queued";
    }
    out->append(std::move(value));
  }
  return true;
}

void handle_workflow_decision_endpoint(const DaemonConfig& cfg, const CorsConfig& cors_cfg,
    AgentDb* db, const HttpRequest& req, HttpResponse* resp) {
  cors_apply(req, resp, cors_cfg); resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  const auto fail = [&](int status, const std::string& message) { resp->status = status; resp->body = json_error_body(message); };
  if (!db || !db->is_open()) { fail(503, "db not available"); return; }
  if (req.body.size() > 8192) { fail(400, "decision request exceeds 8 KiB"); return; }
  Json::Value args; std::string error;
  if (!json_parse_object(req.body, &args, &error) || !fields(args, {"workflow_id", "task_id", "decision_id", "choice_id", "idempotency_key", "note"}) ||
      !bounded_text(args["workflow_id"], 1, 1024) || !bounded_text(args["task_id"], 1, 1024) ||
      !safe_id(args["decision_id"], 128) || !safe_id(args["choice_id"], 64) || !safe_id(args["idempotency_key"], 128) ||
      (args.isMember("note") && !bounded_text(args["note"], 0, 4096))) {
    fail(400, "expected workflow_id, task_id, decision_id, choice_id, idempotency_key and optional note"); return;
  }
  AgentDb::WorkflowDecisionRow expected, saved;
  if (!db->get_workflow_decision(args["workflow_id"].asString(), args["task_id"].asString(), &expected, &error)) {
    fail(error.empty() ? 404 : 500, error.empty() ? "decision not found" : "cannot load decision"); return;
  }
  if (expected.decision_id != args["decision_id"].asString()) { fail(409, "decision identity mismatch"); return; }
  Json::Value spec;
  if (!verified(expected, &spec)) { fail(500, "stored decision integrity check failed"); return; }
  std::string outcome;
  for (const auto& choice : spec["choices"]) if (choice["id"] == args["choice_id"]) outcome = choice["outcome"].asString();
  if (outcome.empty()) { fail(400, "choice is not configured for this decision"); return; }
  const auto result = db->resolve_workflow_decision(expected, args["choice_id"].asString(), outcome,
    args.get("note", "").asString(), args["idempotency_key"].asString(), &saved, &error);
  using Result = AgentDb::WorkflowDecisionResolution;
  if (result != Result::Recorded && result != Result::Replayed) {
    fail(result == Result::StorageError ? 500 : result == Result::NotFound ? 404 : 409,
      result == Result::StorageError ? "cannot record decision" : error); return;
  }
  Json::Value body(Json::objectValue); body["ok"] = true; body["replayed"] = result == Result::Replayed;
  body["decision"] = metadata(saved, spec);
  resp->body = encode(body);
}
}  // namespace agentd
