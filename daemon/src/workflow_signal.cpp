#include "workflow_signal.h"
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
AgentDb::WorkflowSignalRow definition(const std::string& workflow, const std::string& task, const Json::Value& spec) {
  AgentDb::WorkflowSignalRow row;
  row.workflow_id = workflow; row.task_id = task;
  const auto value = normalized(spec);
  row.spec_json = encode(value); row.request_hash = hash(value);
  Json::Value identity(Json::arrayValue); identity.append(workflow); identity.append(task); identity.append(row.request_hash);
  row.signal_id = "ws_" + hash(identity);
  return row;
}
bool verified(const AgentDb::WorkflowSignalRow& row, Json::Value* spec) {
  std::string error;
  if (!json_parse_object(row.spec_json, spec, &error) || !workflow_signal_validate(*spec, &error)) return false;
  const auto expected = definition(row.workflow_id, row.task_id, *spec);
  if (expected.request_hash.empty() || row.request_hash != expected.request_hash || row.signal_id != expected.signal_id) return false;
  if (row.activated_unix_ms < 0 || (!row.activated_unix_ms &&
      (!spec->get("accept_before_activation", false).asBool() || row.expires_unix_ms || row.state == "expired"))) return false;
  if (row.state == "resolved") {
    Json::Value payload;
    return row.payload_json.size() <= 16384 && json_parse_object(row.payload_json, &payload, &error) &&
      !row.payload_hash.empty() && hash(payload) == row.payload_hash;
  }
  return row.state == "waiting" || row.state == "expired";
}
Json::Value metadata(const AgentDb::WorkflowSignalRow& row, const Json::Value& spec) {
  Json::Value out(Json::objectValue);
  out["workflow_id"] = row.workflow_id; out["task_id"] = row.task_id;
  out["signal_id"] = row.signal_id; out["request_hash"] = row.request_hash;
  out["name"] = spec["name"]; out["state"] = row.state;
  out["accept_before_activation"] = spec.get("accept_before_activation", false);
  if (row.activated_unix_ms > 0) out["activated_unix_ms"] = Json::Int64(row.activated_unix_ms);
  else out["state"] = row.state == "resolved" ? "buffered" : "pending";
  if (spec.isMember("title")) out["title"] = spec["title"];
  out["available"] = false;
  out["created_unix_ms"] = Json::Int64(row.created_unix_ms);
  if (row.expires_unix_ms > 0) out["expires_unix_ms"] = Json::Int64(row.expires_unix_ms);
  if (row.resolved_unix_ms > 0) out["resolved_unix_ms"] = Json::Int64(row.resolved_unix_ms);
  if (row.state == "resolved") {
    out["payload_hash"] = row.payload_hash;
  }
  return out;
}
Json::Value failure(const std::string& message) {
  Json::Value out(Json::objectValue);
  out["ok"] = false; out["kind"] = "signal"; out["retryable"] = false;
  out["assistant_text"] = ""; out["error"] = message;
  out["elapsed_ms"] = Json::Int64(0); out["tool_calls_total"] = Json::Int64(0);
  out["steps_executed"] = Json::Int64(0);
  out["usage"]["prompt_tokens"] = Json::Int64(0); out["usage"]["completion_tokens"] = Json::Int64(0);
  out["usage"]["total_tokens"] = Json::Int64(0);
  return out;
}
}

bool workflow_signal_validate(const Json::Value& spec, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const std::string& message) { if (error) *error = "invalid signal: " + message; return false; };
  if (!spec.isObject() || !fields(spec, {"name", "title", "timeout_ms", "accept_before_activation"}))
    return fail("expected name, optional title, timeout_ms and accept_before_activation");
  if (spec.isMember("accept_before_activation") && !spec["accept_before_activation"].isBool())
    return fail("accept_before_activation must be a Boolean");
  if (!safe_id(spec["name"], 128)) return fail("name must be an id-safe string of 1..128 bytes");
  if (spec.isMember("title") && !bounded_text(spec["title"], 1, 256)) return fail("title must be 1..256 bytes without NUL");
  if (spec.isMember("timeout_ms") && (!spec["timeout_ms"].isInt64() || spec["timeout_ms"].asInt64() < 0 ||
      spec["timeout_ms"].asInt64() > 2592000000LL)) return fail("timeout_ms must be an integer from 0 to 2592000000");
  return true;
}

bool workflow_signal_definition(const std::string& workflow, const std::string& task,
    const Json::Value& spec, AgentDb::WorkflowSignalRow* out, std::string* error) {
  if (!out || !workflow_signal_validate(spec, error)) return false;
  *out = definition(workflow, task, spec);
  return !out->request_hash.empty() && !out->signal_id.empty();
}

Json::Value workflow_signal_step(AgentDb* db, const AgentDb::WorkflowTaskRow& task,
    const Json::Value& spec, bool* yielded) {
  if (yielded) *yielded = false;
  std::string error;
  if (!db || !yielded || !workflow_signal_validate(spec, &error)) return failure(error.empty() ? "signal storage unavailable" : error);
  const auto def = definition(task.workflow_id, task.task_id, spec);
  AgentDb::WorkflowSignalRow row;
  if (def.request_hash.empty() || !db->prepare_workflow_signal(task, def, normalized(spec)["timeout_ms"].asInt64(), &row, yielded, &error))
    return failure("cannot persist process signal");
  if (*yielded) return Json::Value();
  if (row.state == "stopped") { auto out = failure("workflow stopped"); out["cancelled"] = true; return out; }
  Json::Value saved;
  if (!verified(row, &saved)) return failure("stored signal integrity check failed");
  auto out = failure(row.state == "expired" ? "signal expired" : "signal has not been delivered");
  out["signal"] = metadata(row, saved);
  if (row.state == "resolved") {
    Json::Value payload;
    if (!json_parse_object(row.payload_json, &payload, &error)) return failure("stored signal payload is invalid");
    out["ok"] = true; out["payload"] = std::move(payload); out["payload_hash"] = row.payload_hash;
    out["assistant_text"] = "Received signal: " + saved["name"].asString();
    out.removeMember("error");
  }
  return out;
}

bool workflow_signals_projection(AgentDb* db, const AgentDb::WorkflowRow& workflow,
    Json::Value* out, std::string* error) {
  if (!db || !out) return false;
  *out = Json::Value(Json::arrayValue);
  std::vector<AgentDb::WorkflowSignalRow> rows;
  if (!db->list_workflow_signals(workflow.workflow_id, &rows, error)) return false;
  if (rows.empty()) return true;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  if (!db->list_workflow_tasks(workflow.workflow_id, &tasks, error)) return false;
  const auto now = now_ms();
  const bool active = !workflow.cancel_requested && (workflow.status == "queued" || workflow.status == "running") &&
    !(workflow.deadline_unix_ms > 0 && workflow.deadline_unix_ms <= now);
  for (const auto& row : rows) {
    Json::Value spec;
    if (!verified(row, &spec)) { if (error) *error = "stored signal integrity check failed"; return false; }
    auto value = metadata(row, spec);
    std::string status;
    for (const auto& task : tasks) if (task.task_id == row.task_id) status = task.status;
    if (!row.activated_unix_ms) {
      if (!active || (status != "queued" && status != "running")) value["state"] = "unused";
      else value["available"] = row.state == "waiting";
    } else if (row.state == "waiting") {
      if (!active || (status != "queued" && status != "running")) value["state"] = "stopped";
      else if (row.expires_unix_ms > 0 && row.expires_unix_ms <= now) value["state"] = "expired";
      else value["available"] = status == "queued";
    }
    out->append(std::move(value));
  }
  return true;
}

void handle_workflow_signal_endpoint(const DaemonConfig& cfg, const CorsConfig& cors_cfg,
    AgentDb* db, const HttpRequest& req, HttpResponse* resp) {
  cors_apply(req, resp, cors_cfg); resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  const auto fail = [&](int status, const std::string& message) { resp->status = status; resp->body = json_error_body(message); };
  if (!db || !db->is_open()) { fail(503, "db not available"); return; }
  if (req.body.size() > 32768) { fail(400, "signal request exceeds 32 KiB"); return; }
  Json::Value args; std::string error;
  if (!json_parse_object(req.body, &args, &error) || !fields(args, {"workflow_id", "task_id", "signal_id", "idempotency_key", "payload"}) ||
      !bounded_text(args["workflow_id"], 1, 1024) || !bounded_text(args["task_id"], 1, 1024) ||
      !safe_id(args["signal_id"], 128) || !safe_id(args["idempotency_key"], 128) ||
      !args["payload"].isObject() || encode(args["payload"]).size() > 16384) {
    fail(400, "expected workflow_id, task_id, signal_id, idempotency_key and payload object of at most 16 KiB"); return;
  }
  AgentDb::WorkflowSignalRow expected, saved;
  if (!db->get_workflow_signal(args["workflow_id"].asString(), args["task_id"].asString(), &expected, &error)) {
    fail(error.empty() ? 404 : 500, error.empty() ? "signal not found" : "cannot load signal"); return;
  }
  if (expected.signal_id != args["signal_id"].asString()) { fail(409, "signal identity mismatch"); return; }
  Json::Value spec;
  if (!verified(expected, &spec)) { fail(500, "stored signal integrity check failed"); return; }
  const auto payload_hash = hash(args["payload"]);
  if (payload_hash.empty()) { fail(400, "cannot canonicalize signal payload"); return; }
  const auto result = db->resolve_workflow_signal(expected, encode(args["payload"]), payload_hash,
    args["idempotency_key"].asString(), &saved, &error);
  using Result = AgentDb::WorkflowSignalResolution;
  if (result != Result::Recorded && result != Result::Replayed) {
    fail(result == Result::StorageError ? 500 : result == Result::NotFound ? 404 : 409,
      result == Result::StorageError ? "cannot record signal" : error); return;
  }
  Json::Value body(Json::objectValue); body["ok"] = true; body["replayed"] = result == Result::Replayed;
  body["signal"] = metadata(saved, spec);
  resp->body = encode(body);
}
}  // namespace agentd
