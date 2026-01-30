#include "toolset_host_internal.h"

#include "session_store.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <chrono>
#include <thread>

namespace host_tools_internal {

#if !defined(AGENT_HAVE_JSONCPP)
agent_status_t tool_ui_wait_event(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  (void)arguments_json;
  return set_result(out_result, "{\"ok\":false,\"error\":\"jsoncpp required\",\"data\":{\"tool\":\"ui_wait_event\"}}");
}
#else

static agent_status_t write_result(agent_string_t* out, const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return set_result(out, Json::writeString(wb, v));
}

static bool event_matches(const Json::Value& payload, const std::string& type, int64_t after_unix_ms, const std::string& path) {
  if (!payload.isObject()) return false;
  const auto& t = payload["type"];
  if (!t.isString() || t.asString() != type) return false;
  const auto& ts = payload["ts_unix_ms"];
  if (after_unix_ms > 0) {
    int64_t v = 0;
    if (ts.isInt64()) v = ts.asInt64();
    else if (ts.isUInt64()) v = (int64_t)ts.asUInt64();
    if (v > 0 && v < after_unix_ms) return false;
  }
  if (!path.empty()) {
    const auto& d = payload["data"];
    if (!d.isObject()) return false;
    const auto& p = d["path"];
    if (!p.isString() || p.asString() != path) return false;
  }
  return true;
}

agent_status_t tool_ui_wait_event(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;

  std::string err;
  Json::Value args;
  if (!parse_json(arguments_json, &args, &err)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err;
    Json::Value d(Json::objectValue);
    d["tool"] = "ui_wait_event";
    o["data"] = d;
    return write_result(out_result, o);
  }

  const std::string type = args.isMember("type") && args["type"].isString() ? args["type"].asString() : "";
  if (type.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing type";
    Json::Value d(Json::objectValue);
    d["tool"] = "ui_wait_event";
    o["data"] = d;
    return write_result(out_result, o);
  }

  int timeout_ms = 30000;
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt()) timeout_ms = args["timeout_ms"].asInt();
  timeout_ms = std::max(0, std::min(timeout_ms, 300000));

  int64_t after_unix_ms = 0;
  if (args.isMember("after_unix_ms")) {
    const auto& v = args["after_unix_ms"];
    if (v.isInt64()) after_unix_ms = v.asInt64();
    else if (v.isUInt64()) after_unix_ms = (int64_t)v.asUInt64();
    else if (v.isInt()) after_unix_ms = (int64_t)v.asInt();
  }

  size_t max_bytes = 256 * 1024;
  if (args.isMember("max_bytes") && args["max_bytes"].isInt()) {
    max_bytes = (size_t)std::max(0, args["max_bytes"].asInt());
  }
  max_bytes = std::min<size_t>(max_bytes, 1024 * 1024);

  const std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";

  if (ctx->sessions_root_dir.empty() || ctx->session_id.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "ui_wait_event requires session context (session_id + sessions_root_dir)";
    Json::Value d(Json::objectValue);
    d["tool"] = "ui_wait_event";
    o["data"] = d;
    return write_result(out_result, o);
  }

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = ctx->sessions_root_dir.string();

  const auto started = std::chrono::steady_clock::now();
  while (true) {
    if (is_cancelled(ctx)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "cancelled";
      Json::Value d(Json::objectValue);
      d["tool"] = "ui_wait_event";
      d["cancelled"] = true;
      o["data"] = d;
      return write_result(out_result, o);
    }

    std::string tail;
    (void)session_store_read_client_event_tail(store_cfg, ctx->session_id, max_bytes, &tail);

    // Parse JSONL and search for the newest matching event.
    Json::Value matched(Json::nullValue);
    int64_t matched_ts = 0;

    Json::CharReaderBuilder rb;
    std::string perr;
    std::istringstream iss(tail);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      Json::Value payload;
      std::istringstream lss(line);
      if (!Json::parseFromStream(rb, lss, &payload, &perr)) {
        continue;
      }
      if (!event_matches(payload, type, after_unix_ms, path)) {
        continue;
      }
      int64_t ts = 0;
      const auto& tsv = payload["ts_unix_ms"];
      if (tsv.isInt64()) ts = tsv.asInt64();
      else if (tsv.isUInt64()) ts = (int64_t)tsv.asUInt64();
      if (ts >= matched_ts) {
        matched_ts = ts;
        matched = payload;
      }
    }

    if (!matched.isNull()) {
      Json::Value o(Json::objectValue);
      o["ok"] = true;
      Json::Value d(Json::objectValue);
      d["tool"] = "ui_wait_event";
      d["timed_out"] = false;
      d["event"] = matched;
      o["data"] = d;
      return write_result(out_result, o);
    }

    if (timeout_ms == 0) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "timeout";
      Json::Value d(Json::objectValue);
      d["tool"] = "ui_wait_event";
      d["timed_out"] = true;
      o["data"] = d;
      return write_result(out_result, o);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= timeout_ms) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "timeout";
      Json::Value d(Json::objectValue);
      d["tool"] = "ui_wait_event";
      d["timed_out"] = true;
      o["data"] = d;
      return write_result(out_result, o);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

#endif

} // namespace host_tools_internal

