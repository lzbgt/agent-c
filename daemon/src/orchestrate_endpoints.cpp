#include "orchestrate_endpoints.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "run_endpoints.h"
#include "session_id_util.h"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agentd {

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

void handle_orchestrate_endpoint(
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
    resp->body = json_stringify_compact(o);
    return;
  }

  const auto& tasks = args["tasks"];
  if (!tasks.isArray() || tasks.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing tasks (expected non-empty array)";
    resp->body = json_stringify_compact(o);
    return;
  }
  if (tasks.size() > 32) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "too many tasks (max 32)";
    resp->body = json_stringify_compact(o);
    return;
  }

  const int max_concurrency =
    args.isMember("max_concurrency") && args["max_concurrency"].isInt()
      ? std::max(1, std::min(args["max_concurrency"].asInt(), 16))
      : 4;
  const bool allow_sessions = args.isMember("allow_sessions") && args["allow_sessions"].isBool() ? args["allow_sessions"].asBool() : false;
  const Json::Value defaults =
    args.isMember("defaults") && args["defaults"].isObject() ? args["defaults"] : Json::Value(Json::nullValue);

  std::string writeback_session_id;
  if (args.isMember("writeback_session_id") && args["writeback_session_id"].isString()) {
    writeback_session_id = args["writeback_session_id"].asString();
  }
  std::string writeback_role = "assistant";
  if (args.isMember("writeback_role") && args["writeback_role"].isString()) {
    writeback_role = args["writeback_role"].asString();
  }
  if (!writeback_session_id.empty() && !session_id_is_safe(writeback_session_id)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid writeback_session_id";
    resp->body = json_stringify_compact(o);
    return;
  }
  if (writeback_role != "assistant" && writeback_role != "user" && writeback_role != "system") {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid writeback_role (expected assistant|user|system)";
    resp->body = json_stringify_compact(o);
    return;
  }

  struct TaskIn {
    std::string task_id;
    std::string body_json;
  };
  std::vector<TaskIn> in;
  in.reserve(tasks.size());

  for (Json::ArrayIndex i = 0; i < tasks.size(); i++) {
    const auto& t = tasks[i];
    if (!t.isObject()) continue;

    const std::string task_id = t.isMember("task_id") && t["task_id"].isString() ? t["task_id"].asString() : ("task_" + std::to_string(i));
    Json::Value run_req = t.isMember("request") && t["request"].isObject() ? t["request"] : t;

    if (defaults.isObject()) {
      for (const auto& k : defaults.getMemberNames()) {
        if (!run_req.isMember(k)) {
          run_req[k] = defaults[k];
        }
      }
    }

    if (!allow_sessions) {
      run_req["no_session"] = true;
      // When orchestrating, tools are powerful; default to tools=none unless explicitly set.
      if (!run_req.isMember("tools")) {
        run_req["tools"] = "none";
      }
    }

    // Ensure a prompt exists.
    if (!run_req.isMember("prompt") || !run_req["prompt"].isString() || run_req["prompt"].asString().empty()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "task missing prompt";
      o["task_id"] = task_id;
      resp->status = 400;
      resp->body = json_stringify_compact(o);
      return;
    }

    in.push_back(TaskIn{task_id, json_stringify_compact(run_req)});
  }

  if (in.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"no valid task objects"})";
    return;
  }

  struct TaskOut {
    std::string task_id;
    bool ok = false;
    Json::Value result = Json::Value(Json::nullValue); // run_request_to_json output
    int http_status = 0;
    int64_t ms = 0;
  };
  std::vector<TaskOut> out;
  out.resize(in.size());

  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      const size_t idx = next.fetch_add(1);
      if (idx >= in.size()) return;
      const auto started = std::chrono::steady_clock::now();
      Json::Value r = run_request_to_json_internal(cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, in[idx].body_json, nullptr);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

      TaskOut to;
      to.task_id = in[idx].task_id;
      to.result = r;
      to.ok = r.isObject() && r.isMember("ok") && r["ok"].isBool() && r["ok"].asBool();
      to.http_status = r.isObject() && r.isMember("http_status") && r["http_status"].isInt() ? r["http_status"].asInt() : 0;
      to.ms = (int64_t)ms;
      out[idx] = std::move(to);
    }
  };

  const int threads = std::min<int>(max_concurrency, (int)in.size());
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (int i = 0; i < threads; i++) {
    pool.emplace_back(worker);
  }
  for (auto& th : pool) {
    if (th.joinable()) th.join();
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["tasks_total"] = (Json::UInt64)out.size();
  o["max_concurrency"] = threads;
  Json::Value arr(Json::arrayValue);
  bool all_ok = true;
  for (const auto& t : out) {
    Json::Value row(Json::objectValue);
    row["task_id"] = t.task_id;
    row["ok"] = t.ok;
    row["ms"] = (Json::Int64)t.ms;
    if (t.http_status) row["http_status"] = t.http_status;
    row["result"] = t.result;
    if (!t.ok) all_ok = false;
    arr.append(row);
  }
  o["all_ok"] = all_ok;
  o["results"] = arr;

  if (!writeback_session_id.empty()) {
    Json::Value wb(Json::objectValue);
    wb["session_id"] = writeback_session_id;
    wb["role"] = writeback_role;

    if (!db_or_null || !db_or_null->is_open()) {
      wb["ok"] = false;
      wb["error"] = "db not available";
    } else {
      std::ostringstream ss;
      ss << "[orchestrate]\n";
      ss << "tasks_total=" << out.size() << " all_ok=" << (all_ok ? "true" : "false") << "\n\n";
      for (const auto& t : out) {
        ss << "- " << t.task_id << ": " << (t.ok ? "ok" : "fail") << " (" << t.ms << "ms)\n";
        if (t.result.isObject()) {
          if (t.result.isMember("assistant_text") && t.result["assistant_text"].isString()) {
            const std::string at = t.result["assistant_text"].asString();
            if (!at.empty()) ss << at << "\n";
          } else if (t.result.isMember("error") && t.result["error"].isString()) {
            ss << "error: " << t.result["error"].asString() << "\n";
          }
        }
        ss << "\n";
        if (ss.tellp() > 32000) {
          ss << "… (truncated)\n";
          break;
        }
      }
      const std::string content = ss.str();

      std::vector<std::pair<std::string, std::string>> msgs;
      std::string err;
      (void)db_or_null->load_session_messages(writeback_session_id, &msgs, &err); // missing session => empty
      msgs.emplace_back(writeback_role, content);
      err.clear();
      const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      if (!db_or_null->replace_session_messages(writeback_session_id, msgs, now_ms, &err)) {
        wb["ok"] = false;
        wb["error"] = err.empty() ? "failed to write back to session" : err;
      } else {
        wb["ok"] = true;
        wb["bytes"] = (Json::UInt64)content.size();
      }
    }
    o["writeback"] = wb;
  }

  resp->body = json_stringify_compact(o);
}

}  // namespace agentd
