#include "toolset_host_internal.h"

#include "session_store.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <chrono>
#include <algorithm>
#include <vector>
#include <thread>

namespace host_tools_internal {

#if !defined(AGENT_HAVE_JSONCPP)
agent_status_t tool_ui_wait_event(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  (void)arguments_json;
  return set_result(out_result, "{\"ok\":false,\"error\":\"jsoncpp required\",\"data\":{\"tool\":\"ui_wait_event\"}}");
}
agent_status_t tool_ui_wait_any(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  (void)arguments_json;
  return set_result(out_result, "{\"ok\":false,\"error\":\"jsoncpp required\",\"data\":{\"tool\":\"ui_wait_any\"}}");
}
agent_status_t tool_ui_wait_all(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  (void)arguments_json;
  return set_result(out_result, "{\"ok\":false,\"error\":\"jsoncpp required\",\"data\":{\"tool\":\"ui_wait_all\"}}");
}
#else

static agent_status_t write_result(agent_string_t* out, const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return set_result(out, Json::writeString(wb, v));
}

static bool json_value_matches(const Json::Value& expected, const Json::Value& actual, int depth, int* budget) {
  if (!budget || *budget <= 0) return false;
  (*budget)--;
  if (depth > 8) return false;

  if (expected.isObject()) {
    if (!actual.isObject()) return false;
    for (const auto& k : expected.getMemberNames()) {
      if (!json_value_matches(expected[k], actual[k], depth + 1, budget)) return false;
    }
    return true;
  }
  if (expected.isArray()) {
    if (!actual.isArray()) return false;
    if (actual.size() != expected.size()) return false;
    for (Json::ArrayIndex i = 0; i < expected.size(); i++) {
      if (!json_value_matches(expected[i], actual[i], depth + 1, budget)) return false;
    }
    return true;
  }
  if (expected.isString()) {
    return actual.isString() && actual.asString() == expected.asString();
  }
  if (expected.isBool()) {
    return actual.isBool() && actual.asBool() == expected.asBool();
  }
  if (expected.isNull()) {
    return actual.isNull();
  }
  if (expected.isDouble()) {
    if (!(actual.isDouble() || actual.isInt() || actual.isInt64() || actual.isUInt() || actual.isUInt64())) return false;
    return actual.asDouble() == expected.asDouble();
  }
  if (expected.isInt() || expected.isInt64()) {
    const int64_t ex = expected.isInt64() ? expected.asInt64() : (int64_t)expected.asInt();
    int64_t av = 0;
    if (actual.isInt64()) av = actual.asInt64();
    else if (actual.isUInt64()) av = (int64_t)actual.asUInt64();
    else if (actual.isInt()) av = (int64_t)actual.asInt();
    else if (actual.isUInt()) av = (int64_t)actual.asUInt();
    else return false;
    return av == ex;
  }
  if (expected.isUInt() || expected.isUInt64()) {
    const uint64_t ex = expected.isUInt64() ? expected.asUInt64() : (uint64_t)expected.asUInt();
    uint64_t av = 0;
    if (actual.isUInt64()) av = actual.asUInt64();
    else if (actual.isInt64()) {
      const int64_t v = actual.asInt64();
      if (v < 0) return false;
      av = (uint64_t)v;
    } else if (actual.isUInt()) av = (uint64_t)actual.asUInt();
    else if (actual.isInt()) {
      const int v = actual.asInt();
      if (v < 0) return false;
      av = (uint64_t)v;
    } else return false;
    return av == ex;
  }
  // Unknown expected type: fail closed to avoid surprising matches.
  return false;
}

static bool data_matches(const Json::Value& payload, const Json::Value& data_match) {
  if (!data_match.isObject()) return true;
  if (!payload.isObject()) return false;
  const auto& d = payload["data"];
  if (!d.isObject()) return false;
  int budget = 256;
  return json_value_matches(data_match, d, 0, &budget);
}

static bool event_matches(
  const Json::Value& payload,
  const std::string& type,
  int64_t after_unix_ms,
  const std::string& path,
  const Json::Value& data_match
) {
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
  if (!data_matches(payload, data_match)) return false;
  return true;
}

struct UiWaitPredicate {
  std::string type;
  int64_t after_unix_ms = 0;
  std::string path;
  Json::Value data_match = Json::Value(Json::nullValue);
};

static bool parse_predicate(const Json::Value& v, UiWaitPredicate* out, std::string* out_err) {
  if (!out) return false;
  *out = UiWaitPredicate{};
  if (!v.isObject()) {
    if (out_err) *out_err = "predicate must be object";
    return false;
  }
  out->type = v.isMember("type") && v["type"].isString() ? v["type"].asString() : "";
  if (out->type.empty()) {
    if (out_err) *out_err = "predicate missing type";
    return false;
  }
  if (v.isMember("after_unix_ms")) {
    const auto& a = v["after_unix_ms"];
    if (a.isInt64()) out->after_unix_ms = a.asInt64();
    else if (a.isUInt64()) out->after_unix_ms = (int64_t)a.asUInt64();
    else if (a.isInt()) out->after_unix_ms = (int64_t)a.asInt();
  }
  out->path = v.isMember("path") && v["path"].isString() ? v["path"].asString() : "";
  out->data_match = v.isMember("data_match") && (v["data_match"].isObject() || v["data_match"].isArray())
    ? v["data_match"]
    : Json::Value(Json::nullValue);
  return true;
}

static agent_status_t read_client_events_tail(const HostToolCtx* ctx, size_t max_bytes, size_t max_files, std::string* out_tail) {
  if (!out_tail) return AGENT_ERR_INVALID_ARGUMENT;
  out_tail->clear();
  if (!ctx) return AGENT_ERR_INVALID_ARGUMENT;
  if (ctx->sessions_root_dir.empty() || ctx->session_id.empty()) return AGENT_ERR_INVALID_ARGUMENT;

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = ctx->sessions_root_dir.string();
  if (max_files == 0) max_files = store_cfg.client_events_max_files;
  max_files = std::min<size_t>(max_files, 10);
  return session_store_read_client_event_tail_multi(store_cfg, ctx->session_id, max_bytes, max_files, out_tail);
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
  const Json::Value data_match = args.isMember("data_match") && args["data_match"].isObject() ? args["data_match"] : Json::Value(Json::nullValue);

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
    (void)session_store_read_client_event_tail_multi(store_cfg, ctx->session_id, max_bytes, store_cfg.client_events_max_files, &tail);

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
      if (!event_matches(payload, type, after_unix_ms, path, data_match)) {
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

static agent_status_t ui_wait_join_impl(
  HostToolCtx* ctx,
  const char* tool_name,
  const char* arguments_json,
  bool require_all,
  agent_string_t* out_result
) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;

  std::string err;
  Json::Value args;
  if (!parse_json(arguments_json, &args, &err)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err;
    Json::Value d(Json::objectValue);
    d["tool"] = tool_name;
    o["data"] = d;
    return write_result(out_result, o);
  }

  int timeout_ms = 30000;
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt()) timeout_ms = args["timeout_ms"].asInt();
  timeout_ms = std::max(0, std::min(timeout_ms, 300000));

  size_t max_bytes = 256 * 1024;
  if (args.isMember("max_bytes") && args["max_bytes"].isInt()) {
    max_bytes = (size_t)std::max(0, args["max_bytes"].asInt());
  }
  max_bytes = std::min<size_t>(max_bytes, 1024 * 1024);

  size_t max_files = 0;
  if (args.isMember("max_files") && args["max_files"].isInt()) {
    max_files = (size_t)std::max(0, args["max_files"].asInt());
  }

  const auto& preds = args["predicates"];
  if (!preds.isArray() || preds.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing predicates";
    Json::Value d(Json::objectValue);
    d["tool"] = tool_name;
    o["data"] = d;
    return write_result(out_result, o);
  }
  if (preds.size() > 16) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "too many predicates (max 16)";
    Json::Value d(Json::objectValue);
    d["tool"] = tool_name;
    o["data"] = d;
    return write_result(out_result, o);
  }

  if (ctx->sessions_root_dir.empty() || ctx->session_id.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string(tool_name) + " requires session context (session_id + sessions_root_dir)";
    Json::Value d(Json::objectValue);
    d["tool"] = tool_name;
    o["data"] = d;
    return write_result(out_result, o);
  }

  std::vector<UiWaitPredicate> predicates;
  predicates.reserve((size_t)preds.size());
  for (Json::ArrayIndex i = 0; i < preds.size(); i++) {
    UiWaitPredicate p;
    std::string perr;
    if (!parse_predicate(preds[i], &p, &perr)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid predicate: " + perr;
      Json::Value d(Json::objectValue);
      d["tool"] = tool_name;
      d["predicate_index"] = (Json::UInt64)i;
      o["data"] = d;
      return write_result(out_result, o);
    }
    predicates.push_back(std::move(p));
  }

  std::vector<uint8_t> matched(predicates.size(), 0);
  std::vector<Json::Value> matched_events(predicates.size(), Json::Value(Json::nullValue));

  const auto started = std::chrono::steady_clock::now();
  while (true) {
    if (is_cancelled(ctx)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "cancelled";
      Json::Value d(Json::objectValue);
      d["tool"] = tool_name;
      d["cancelled"] = true;
      o["data"] = d;
      return write_result(out_result, o);
    }

    std::string tail;
    (void)read_client_events_tail(ctx, max_bytes, max_files, &tail);

    // Scan tail once, try to satisfy predicates.
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

      for (size_t i = 0; i < predicates.size(); i++) {
        if (matched[i]) continue;
        const UiWaitPredicate& p = predicates[i];
        if (!event_matches(payload, p.type, p.after_unix_ms, p.path, p.data_match)) continue;
        matched[i] = 1;
        matched_events[i] = payload;
        if (!require_all) {
          Json::Value o(Json::objectValue);
          o["ok"] = true;
          Json::Value d(Json::objectValue);
          d["tool"] = tool_name;
          d["timed_out"] = false;
          d["index"] = (Json::UInt64)i;
          d["event"] = payload;
          o["data"] = d;
          return write_result(out_result, o);
        }
      }
    }

    if (require_all) {
      bool all = true;
      for (size_t i = 0; i < matched.size(); i++) {
        if (!matched[i]) {
          all = false;
          break;
        }
      }
      if (all) {
        Json::Value o(Json::objectValue);
        o["ok"] = true;
        Json::Value d(Json::objectValue);
        d["tool"] = tool_name;
        d["timed_out"] = false;
        Json::Value me(Json::arrayValue);
        Json::Value mm(Json::arrayValue);
        for (size_t i = 0; i < matched_events.size(); i++) {
          mm.append((bool)matched[i]);
          me.append(matched_events[i]);
        }
        d["matched"] = mm;
        d["events"] = me;
        o["data"] = d;
        return write_result(out_result, o);
      }
    }

    if (timeout_ms == 0) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "timeout";
      Json::Value d(Json::objectValue);
      d["tool"] = tool_name;
      d["timed_out"] = true;
      if (require_all) {
        Json::Value mm(Json::arrayValue);
        for (size_t i = 0; i < matched.size(); i++) mm.append((bool)matched[i]);
        d["matched"] = mm;
      }
      o["data"] = d;
      return write_result(out_result, o);
    }

    const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= timeout_ms) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "timeout";
      Json::Value d(Json::objectValue);
      d["tool"] = tool_name;
      d["timed_out"] = true;
      if (require_all) {
        Json::Value mm(Json::arrayValue);
        for (size_t i = 0; i < matched.size(); i++) mm.append((bool)matched[i]);
        d["matched"] = mm;
      }
      o["data"] = d;
      return write_result(out_result, o);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

agent_status_t tool_ui_wait_any(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  return ui_wait_join_impl(ctx, "ui_wait_any", arguments_json, false, out_result);
}

agent_status_t tool_ui_wait_all(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  return ui_wait_join_impl(ctx, "ui_wait_all", arguments_json, true, out_result);
}

#endif

} // namespace host_tools_internal
