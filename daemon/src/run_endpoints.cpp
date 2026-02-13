#include "run_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "run_endpoints_internal.h"
#include "string_util.h"
#include "trace_id_util.h"

#include <json/json.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace agentd {
namespace {

static std::string trace_id_hint_from_header(const HttpRequest& req) {
  std::string tid = trim_copy(header_get_ci(req.headers, "x-trace-id"));
  if (tid.empty()) return "";
  if (!trace_id_is_safe(tid)) return "";
  return tid;
}

static std::string inject_trace_id_if_missing(const std::string& request_body, const std::string& trace_id_hint) {
  if (trace_id_hint.empty()) return request_body;
  Json::Value args;
  std::string perr;
  if (!json_parse_object(request_body, &args, &perr)) {
    return request_body;
  }
  std::string trace_id;
  if (args.isMember("trace_id") && args["trace_id"].isString()) trace_id = trim_copy(args["trace_id"].asString());
  if (!trace_id.empty()) return request_body;
  args["trace_id"] = trace_id_hint;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, args);
}

}  // namespace

void handle_run_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto started = std::chrono::steady_clock::now();
  const std::string trace_id_hint = trace_id_hint_from_header(req);
  const std::string body = inject_trace_id_if_missing(req.body, trace_id_hint);
  std::cerr << "agentd: /api/v1/run start bytes=" << req.body.size() << "\n";
  Json::Value out =
    run_request_to_json_internal(cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, body, nullptr);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  const bool ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
  std::cerr << "agentd: /api/v1/run done ok=" << (ok ? "true" : "false") << " ms=" << ms << "\n";
  if (out.isObject() && out.isMember("rpc_status") && out["rpc_status"].isInt()) {
    resp->status = out["rpc_status"].asInt();
  }
  if (out.isObject() && out.isMember("trace_id") && out["trace_id"].isString()) {
    resp->headers["X-Trace-Id"] = out["trace_id"].asString();
  }
  resp->body = json_stringify(out);
}

void handle_run_async_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }
  const std::string prompt = args.isMember("prompt") && args["prompt"].isString() ? args["prompt"].asString() : "";
  if (prompt.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing prompt";
    resp->body = json_stringify(o);
    return;
  }

  std::string trace_id;
  if (args.isMember("trace_id") && args["trace_id"].isString()) trace_id = trim_copy(args["trace_id"].asString());
  if (trace_id.empty()) trace_id = trace_id_hint_from_header(req);
  if (!trace_id.empty() && !trace_id_is_safe(trace_id)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid trace_id";
    resp->body = json_stringify(o);
    return;
  }
  if (trace_id.empty()) trace_id = make_uuidish_trace_id();
  args["trace_id"] = trace_id;
  resp->headers["X-Trace-Id"] = trace_id;

  const std::string job_id = args.isMember("job_id") && args["job_id"].isString() ? args["job_id"].asString() : new_job_id();
  if (job_id.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"empty job_id"})";
    return;
  }

  int priority = 0;
  if (args.isMember("priority")) {
    if (!args["priority"].isInt()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid priority (expected int)\"}";
      return;
    }
    priority = args["priority"].asInt();
    if (priority < -1000) priority = -1000;
    if (priority > 1000) priority = 1000;
    args["priority"] = priority; // canonicalize
  }
  if (!args.isMember("priority")) args["priority"] = priority;
  if (!job_create(job_id)) {
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "job_id already exists";
    o["job_id"] = job_id;
    resp->body = json_stringify(o);
    return;
  }
  job_set_trace_id(job_id, trace_id);

  // Create the canonical request body for execution and a redacted copy for persistence.
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string body_copy = Json::writeString(wb, args);
  const std::string body_persist = sanitize_job_request_json_for_persist(body_copy);
  if (body_persist.empty()) {
    std::cerr << "agentd: warning: run_async job request too large to persist safely; job will not be resumable after restart"
              << " job=" << job_id << " bytes=" << body_copy.size() << "\n";
  }

  // Persist a durable job stub so UIs can still inspect job state after daemon restart.
  const std::string session_id =
    args.isMember("session_id") && args["session_id"].isString() ? args["session_id"].asString() : std::string("default");
  const bool no_session = args.isMember("no_session") && args["no_session"].isBool() ? args["no_session"].asBool() : false;
  const int64_t created_ms = now_unix_ms();
  if (db_or_null && db_or_null->is_open()) {
    AgentDb::JobRow jr;
    jr.job_id = job_id;
    jr.session_id = no_session ? session_id : session_id;
    jr.trace_id = trace_id;
    jr.request_json = body_persist;
    jr.priority = priority;
    jr.created_unix_ms = created_ms;
    jr.updated_unix_ms = created_ms;
    jr.status = "queued";
    jr.cancel_requested = false;
    jr.error.clear();
    jr.stop_reason.clear();
    jr.result_json.clear();
    jr.last_heartbeat_unix_ms = 0;
    std::string db_err;
    if (!db_or_null->upsert_job(jr, &db_err)) {
      std::cerr << "agentd: warning: failed to persist job row: " << db_err << " job=" << job_id << "\n";
    }
  }

  // Log immediately in the request handler (before the background thread starts).
  // This helps diagnose "hangs" where the UI is pointed at the wrong daemon base URL,
  // or where the request never reaches the daemon.
  std::cerr << "agentd: /api/v1/run_async accepted job=" << job_id << " trace_id=" << trace_id << " bytes=" << req.body.size() << "\n";

  std::thread([job_id, body_copy, cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, session_id, created_ms, priority]() mutable {
    const auto started = std::chrono::steady_clock::now();
    std::cerr << "agentd: /api/v1/run_async job=" << job_id << " start bytes=" << body_copy.size() << "\n";
    job_set_status(job_id, "running", "");
    if (db_or_null && db_or_null->is_open()) {
      AgentDb::JobRow jr;
      jr.job_id = job_id;
      jr.session_id = session_id;
      jr.priority = priority;
      jr.created_unix_ms = created_ms;
      jr.updated_unix_ms = now_unix_ms();
      jr.status = "running";
      jr.cancel_requested = false;
      jr.error.clear();
      jr.stop_reason.clear();
      jr.result_json.clear();
      jr.last_heartbeat_unix_ms = 0;
      std::string db_err;
      if (!db_or_null->upsert_job(jr, &db_err)) {
        std::cerr << "agentd: warning: failed to update job row (running): " << db_err << " job=" << job_id << "\n";
      }
    }
    {
      // Emit an immediate event so UIs don't look "stuck" even if the first LLM request is slow
      // or if the run uses tools="none" (no tool-loop events until completion).
      Json::Value d(Json::objectValue);
      d["source"] = "daemon";
      d["job_id"] = job_id;
      d["status"] = "running";
      d["ts_unix_ms"] = (Json::Int64)now_unix_ms();
      job_append_event(job_id, "start", json_stringify(d));
    }
    try {
      Json::Value out = run_request_to_json_internal(
        cfg,
        ocfg,
        db_or_null,
        tool_ext_or_null,
        sessions_root_dir,
        body_copy,
        job_id.c_str()
      );
      job_set_result(job_id, out);

      if (db_or_null && db_or_null->is_open()) {
        const bool ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
        const bool cancelled =
          out.isObject() && out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool();
        const std::string status = cancelled ? "cancelled" : (ok ? "done" : "error");
        std::string error;
        if (!ok && out.isObject() && out.isMember("error") && out["error"].isString()) error = out["error"].asString();
        if (cancelled) error = "cancelled";

        std::string stop_reason = ok ? "done" : "error";
        std::string last_err_reason;
        if (out.isObject() && out.isMember("events") && out["events"].isArray()) {
          for (Json::ArrayIndex i = 0; i < out["events"].size(); i++) {
            const auto& ev = out["events"][i];
            if (!ev.isObject()) continue;
            const auto& t = ev["type"];
            const auto& d = ev["data"];
            if (!t.isString() || !d.isObject()) continue;
            if (t.asString() == "error" && d.isMember("reason") && d["reason"].isString()) {
              last_err_reason = d["reason"].asString();
            }
            if (t.asString() == "cancelled") {
              if (d.isMember("reason") && d["reason"].isString()) stop_reason = d["reason"].asString();
              else stop_reason = "cancelled";
            }
          }
        }
        if (!ok && !last_err_reason.empty()) stop_reason = last_err_reason;

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        AgentDb::JobRow jr;
        jr.job_id = job_id;
        jr.session_id = session_id;
        jr.created_unix_ms = created_ms;
        jr.updated_unix_ms = now_unix_ms();
        jr.status = status;
        jr.cancel_requested = false;
        jr.error = error;
        jr.stop_reason = stop_reason;
        jr.result_json = Json::writeString(wb, out);
        jr.last_heartbeat_unix_ms = 0;
        std::string db_err;
        if (!db_or_null->upsert_job(jr, &db_err)) {
          std::cerr << "agentd: warning: failed to persist job result: " << db_err << " job=" << job_id << "\n";
        }
      }
    } catch (const std::exception& e) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string("uncaught exception: ") + e.what();
      job_set_result(job_id, o);

      if (db_or_null && db_or_null->is_open()) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        AgentDb::JobRow jr;
        jr.job_id = job_id;
        jr.session_id = session_id;
        jr.created_unix_ms = created_ms;
        jr.updated_unix_ms = now_unix_ms();
        jr.status = "error";
        jr.cancel_requested = false;
        jr.error = std::string("uncaught exception: ") + e.what();
        jr.stop_reason = "exception";
        jr.result_json = Json::writeString(wb, o);
        jr.last_heartbeat_unix_ms = 0;
        std::string db_err;
        (void)db_or_null->upsert_job(jr, &db_err);
      }
    } catch (...) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "uncaught unknown exception";
      job_set_result(job_id, o);

      if (db_or_null && db_or_null->is_open()) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        AgentDb::JobRow jr;
        jr.job_id = job_id;
        jr.session_id = session_id;
        jr.created_unix_ms = created_ms;
        jr.updated_unix_ms = now_unix_ms();
        jr.status = "error";
        jr.cancel_requested = false;
        jr.error = "uncaught unknown exception";
        jr.stop_reason = "exception";
        jr.result_json = Json::writeString(wb, o);
        jr.last_heartbeat_unix_ms = 0;
        std::string db_err;
        (void)db_or_null->upsert_job(jr, &db_err);
      }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    JobState s;
    const bool got = job_get(job_id, &s);
    const bool ok = got && s.result.isObject() && s.result.isMember("ok") && s.result["ok"].isBool() && s.result["ok"].asBool();
    std::cerr << "agentd: /api/v1/run_async job=" << job_id << " done ok=" << (ok ? "true" : "false") << " ms=" << ms << "\n";
  }).detach();

  resp->status = 202;
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = job_id;
  o["trace_id"] = trace_id;
  resp->body = json_stringify(o);
}

}  // namespace agentd
