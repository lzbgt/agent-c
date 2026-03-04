#include "workflow_schedule_endpoints.h"

#include "cron_util.h"
#include "daemon_auth.h"
#include "drain_state.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoint_util.h"

#include <json/json.h>

#include <chrono>
#include <string>
#include <utility>

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static std::string normalize_timezone_or_empty(const std::string& tz) {
  const std::string t = trim_copy(tz);
  if (t.empty()) return "";
  const std::string l = lower_copy(t);
  if (l == "utc" || l == "etc/utc" || l == "gmt") return "UTC";
  return "";
}

static Json::Value schedule_row_to_json(const AgentDb::WorkflowScheduleRow& r) {
  Json::Value o(Json::objectValue);
  o["schedule_id"] = r.schedule_id;
  o["status"] = r.status;
  o["cron"] = r.cron;
  o["timezone"] = r.timezone;
  o["created_unix_ms"] = (Json::Int64)r.created_unix_ms;
  o["updated_unix_ms"] = (Json::Int64)r.updated_unix_ms;
  o["last_tick_unix_ms"] = (Json::Int64)r.last_tick_unix_ms;
  o["next_tick_unix_ms"] = (Json::Int64)r.next_tick_unix_ms;
  if (!r.last_error.empty()) o["last_error"] = r.last_error;
  if (!r.metadata_json.empty()) {
    Json::Value meta;
    std::string perr;
    if (json_parse_object(r.metadata_json, &meta, &perr)) {
      o["metadata"] = std::move(meta);
    }
  }
  return o;
}

static Json::Value schedule_run_row_to_json(const AgentDb::WorkflowScheduleRunRow& r) {
  Json::Value o(Json::objectValue);
  o["schedule_id"] = r.schedule_id;
  o["tick_unix_ms"] = (Json::Int64)r.tick_unix_ms;
  o["workflow_id"] = r.workflow_id;
  o["created_unix_ms"] = (Json::Int64)r.created_unix_ms;
  o["status"] = r.status;
  if (!r.error.empty()) o["error"] = r.error;
  return o;
}

}  // namespace

void handle_workflow_schedule_create_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (drain_is_active()) {
    resp->status = 503;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "draining";
    const int64_t until_ms = drain_until_unix_ms();
    if (until_ms > 0) o["drain_until_unix_ms"] = (Json::Int64)until_ms;
    const std::string reason = drain_reason();
    if (!reason.empty()) o["drain_reason"] = reason;
    resp->body = json_stringify_compact(o);
    return;
  }

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  const std::string cron =
    args.isMember("cron") && args["cron"].isString() ? trim_copy(args["cron"].asString()) : "";
  if (cron.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing cron");
    return;
  }

  const std::string tz_raw =
    args.isMember("timezone") && args["timezone"].isString() ? args["timezone"].asString() : "UTC";
  const std::string tz = normalize_timezone_or_empty(tz_raw);
  if (tz.empty()) {
    resp->status = 400;
    resp->body = json_error_body("unsupported timezone (UTC only)");
    return;
  }

  if (!args.isMember("spec") || !args["spec"].isObject()) {
    resp->status = 400;
    resp->body = json_error_body("missing spec (expected object)");
    return;
  }
  const Json::Value spec = args["spec"];
  if (!spec.isMember("tasks") || !spec["tasks"].isArray() || spec["tasks"].empty()) {
    resp->status = 400;
    resp->body = json_error_body("invalid spec (missing non-empty tasks)");
    return;
  }

  CronSchedule sched;
  std::string cerr;
  if (!cron_parse_5(cron, &sched, &cerr)) {
    resp->status = 400;
    resp->body = json_error_body(cerr.empty() ? "invalid cron" : cerr);
    return;
  }

  const int64_t now = now_unix_ms();
  int64_t next_tick = 0;
  std::string nerr;
  if (!cron_next_tick_utc(sched, now, &next_tick, &nerr)) {
    resp->status = 400;
    resp->body = json_error_body(nerr.empty() ? "failed to compute next tick" : nerr);
    return;
  }

  std::string schedule_id =
    args.isMember("schedule_id") && args["schedule_id"].isString() ? trim_copy(args["schedule_id"].asString()) : "";
  if (schedule_id.empty()) schedule_id = new_workflow_schedule_id();
  if (!id_is_safe(schedule_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid schedule_id");
    return;
  }

  std::string metadata_json;
  if (args.isMember("metadata")) {
    if (!args["metadata"].isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid metadata (expected object)");
      return;
    }
    metadata_json = json_stringify_compact(args["metadata"]);
  }

  AgentDb::WorkflowScheduleRow row;
  row.schedule_id = schedule_id;
  row.created_unix_ms = now;
  row.updated_unix_ms = now;
  row.status = "active";
  row.cron = cron;
  row.timezone = tz;
  row.spec_json = json_stringify_compact(spec);
  row.metadata_json = metadata_json;
  row.last_tick_unix_ms = 0;
  row.next_tick_unix_ms = next_tick;

  std::string derr;
  if (!db_or_null->insert_workflow_schedule(row, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to insert schedule";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule_id"] = schedule_id;
  o["status"] = row.status;
  o["next_tick_unix_ms"] = (Json::Int64)next_tick;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const std::string status = query_get(req.query, "status").value_or("");
  const std::string limit_s = query_get(req.query, "limit").value_or("");
  const std::string offset_s = query_get(req.query, "offset").value_or("");
  size_t limit = 100;
  size_t offset = 0;
  if (!limit_s.empty()) {
    try { limit = (size_t)std::stoul(limit_s); } catch (...) {}
  }
  if (!offset_s.empty()) {
    try { offset = (size_t)std::stoul(offset_s); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(1000, limit));

  std::vector<AgentDb::WorkflowScheduleRow> rows;
  std::string derr;
  if (!db_or_null->list_workflow_schedules(status, limit, offset, &rows, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list schedules";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["status"] = status.empty() ? Json::Value(Json::nullValue) : Json::Value(status);
  Json::Value arr(Json::arrayValue);
  for (const auto& r : rows) arr.append(schedule_row_to_json(r));
  o["schedules"] = std::move(arr);
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto schedule_id = query_get(req.query, "schedule_id");
  if (!schedule_id || schedule_id->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing schedule_id");
    return;
  }

  AgentDb::WorkflowScheduleRow row;
  std::string derr;
  if (!db_or_null->get_workflow_schedule(*schedule_id, &row, &derr)) {
    if (!derr.empty()) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to get schedule";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
    resp->status = 404;
    resp->body = json_error_body("schedule not found");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule"] = schedule_row_to_json(row);
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto schedule_id = query_get(req.query, "schedule_id");
  if (!schedule_id || schedule_id->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing schedule_id");
    return;
  }

  bool found = false;
  std::string derr;
  if (!db_or_null->delete_workflow_schedule(*schedule_id, &found, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to delete schedule";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }
  if (!found) {
    resp->status = 404;
    resp->body = json_error_body("schedule not found");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule_id"] = *schedule_id;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_pause_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }
  const std::string schedule_id =
    args.isMember("schedule_id") && args["schedule_id"].isString() ? trim_copy(args["schedule_id"].asString()) : "";
  if (schedule_id.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing schedule_id");
    return;
  }

  bool found = false;
  std::string derr;
  if (!db_or_null->update_workflow_schedule_status(schedule_id, "paused", now_unix_ms(), &found, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to pause schedule";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }
  if (!found) {
    resp->status = 404;
    resp->body = json_error_body("schedule not found");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule_id"] = schedule_id;
  o["status"] = "paused";
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_resume_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }
  const std::string schedule_id =
    args.isMember("schedule_id") && args["schedule_id"].isString() ? trim_copy(args["schedule_id"].asString()) : "";
  if (schedule_id.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing schedule_id");
    return;
  }

  AgentDb::WorkflowScheduleRow row;
  std::string derr;
  if (!db_or_null->get_workflow_schedule(schedule_id, &row, &derr)) {
    if (!derr.empty()) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to read schedule";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
    resp->status = 404;
    resp->body = json_error_body("schedule not found");
    return;
  }

  const std::string tz = normalize_timezone_or_empty(row.timezone);
  if (tz.empty()) {
    resp->status = 400;
    resp->body = json_error_body("unsupported timezone (UTC only)");
    return;
  }

  CronSchedule sched;
  std::string cerr;
  if (!cron_parse_5(row.cron, &sched, &cerr)) {
    resp->status = 400;
    resp->body = json_error_body(cerr.empty() ? "invalid cron" : cerr);
    return;
  }
  int64_t next_tick = 0;
  std::string nerr;
  const int64_t now = now_unix_ms();
  if (!cron_next_tick_utc(sched, now, &next_tick, &nerr)) {
    resp->status = 400;
    resp->body = json_error_body(nerr.empty() ? "failed to compute next tick" : nerr);
    return;
  }

  bool found = false;
  if (!db_or_null->update_workflow_schedule_status(schedule_id, "active", now, &found, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to resume schedule";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }
  if (!found) {
    resp->status = 404;
    resp->body = json_error_body("schedule not found");
    return;
  }
  (void)db_or_null->update_workflow_schedule_ticks(schedule_id, row.last_tick_unix_ms, next_tick, "", now, &found, &derr);

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule_id"] = schedule_id;
  o["status"] = "active";
  o["next_tick_unix_ms"] = (Json::Int64)next_tick;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_schedule_runs_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto schedule_id = query_get(req.query, "schedule_id");
  if (!schedule_id || schedule_id->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing schedule_id");
    return;
  }

  const std::string limit_s = query_get(req.query, "limit").value_or("");
  const std::string offset_s = query_get(req.query, "offset").value_or("");
  size_t limit = 100;
  size_t offset = 0;
  if (!limit_s.empty()) {
    try { limit = (size_t)std::stoul(limit_s); } catch (...) {}
  }
  if (!offset_s.empty()) {
    try { offset = (size_t)std::stoul(offset_s); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(1000, limit));

  std::vector<AgentDb::WorkflowScheduleRunRow> rows;
  std::string derr;
  if (!db_or_null->list_workflow_schedule_runs(*schedule_id, limit, offset, &rows, &derr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list schedule runs";
    if (!derr.empty()) o["detail"] = derr;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["schedule_id"] = *schedule_id;
  Json::Value arr(Json::arrayValue);
  for (const auto& r : rows) arr.append(schedule_run_row_to_json(r));
  o["runs"] = std::move(arr);
  resp->body = json_stringify_compact(o);
}

}  // namespace agentd
