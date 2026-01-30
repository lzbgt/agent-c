#include "toolset_host_internal.h"

#include "session_store.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace host_tools_internal {

#if !defined(AGENT_HAVE_JSONCPP)
agent_status_t tool_client_peek(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  (void)arguments_json;
  return set_result(out_result, "{\"ok\":false,\"error\":\"jsoncpp required\",\"data\":{\"tool\":\"client_peek\"}}");
}
#else

static agent_status_t write_result(agent_string_t* out, const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return set_result(out, Json::writeString(wb, v));
}

static Json::Value summarize_data(const Json::Value& data, size_t max_keys) {
  Json::Value s(Json::objectValue);
  if (data.isNull()) {
    s["kind"] = "null";
    return s;
  }
  if (data.isString()) {
    s["kind"] = "string";
    s["bytes"] = (Json::UInt64)data.asString().size();
    return s;
  }
  if (data.isBool()) {
    s["kind"] = "bool";
    s["value"] = data.asBool();
    return s;
  }
  if (data.isDouble() || data.isInt() || data.isInt64() || data.isUInt() || data.isUInt64()) {
    s["kind"] = "number";
    s["value"] = data;
    return s;
  }
  if (data.isArray()) {
    s["kind"] = "array";
    s["size"] = (Json::UInt64)data.size();
    return s;
  }
  if (data.isObject()) {
    s["kind"] = "object";
    Json::Value keys(Json::arrayValue);
    const auto names = data.getMemberNames();
    for (size_t i = 0; i < names.size() && i < max_keys; i++) {
      keys.append(names[i]);
    }
    s["keys"] = keys;
    s["total_keys"] = (Json::UInt64)names.size();
    return s;
  }
  s["kind"] = "unknown";
  return s;
}

agent_status_t tool_client_peek(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;

  std::string err;
  Json::Value args;
  if (!parse_json(arguments_json, &args, &err)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err;
    Json::Value d(Json::objectValue);
    d["tool"] = "client_peek";
    o["data"] = d;
    return write_result(out_result, o);
  }

  if (ctx->sessions_root_dir.empty() || ctx->session_id.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "client_peek requires session context (session_id + sessions_root_dir)";
    Json::Value d(Json::objectValue);
    d["tool"] = "client_peek";
    o["data"] = d;
    return write_result(out_result, o);
  }

  size_t max_bytes = 256 * 1024;
  if (args.isMember("max_bytes") && args["max_bytes"].isInt()) {
    max_bytes = (size_t)std::max(0, args["max_bytes"].asInt());
  }
  max_bytes = std::min<size_t>(max_bytes, 1024 * 1024);

  size_t max_files = 0;
  if (args.isMember("max_files") && args["max_files"].isInt()) {
    max_files = (size_t)std::max(0, args["max_files"].asInt());
  }
  max_files = std::min<size_t>(max_files, 10);

  const std::string only_client_id = args.isMember("client_id") && args["client_id"].isString() ? args["client_id"].asString() : "";
  const bool include_data = args.isMember("include_data") && args["include_data"].isBool() ? args["include_data"].asBool() : false;
  size_t max_data_bytes = 8192;
  if (args.isMember("max_data_bytes") && args["max_data_bytes"].isInt()) {
    max_data_bytes = (size_t)std::max(0, args["max_data_bytes"].asInt());
  }
  max_data_bytes = std::min<size_t>(max_data_bytes, 64 * 1024);

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = ctx->sessions_root_dir.string();
  if (max_files == 0) max_files = store_cfg.client_events_max_files;

  std::string tail;
  (void)session_store_read_client_event_tail_multi(store_cfg, ctx->session_id, max_bytes, max_files, &tail);

  struct SeenClient {
    std::string id;
    std::string kind;
    std::string instance_id;
    int64_t ts_unix_ms = 0;
    std::string last_type;
    Json::Value last_event = Json::Value(Json::nullValue);
  };
  std::unordered_map<std::string, SeenClient> seen;

  Json::CharReaderBuilder rb;
  std::string perr;
  std::istringstream iss(tail);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    Json::Value payload;
    std::istringstream lss(line);
    if (!Json::parseFromStream(rb, lss, &payload, &perr) || !payload.isObject()) {
      continue;
    }
    const auto& c = payload["client"];
    if (!c.isObject()) continue;
    const std::string cid = c.isMember("id") && c["id"].isString() ? c["id"].asString() : "";
    if (cid.empty()) continue;
    if (!only_client_id.empty() && cid != only_client_id) continue;

    const std::string kind = c.isMember("kind") && c["kind"].isString() ? c["kind"].asString() : "";
    const std::string inst = c.isMember("instance_id") && c["instance_id"].isString() ? c["instance_id"].asString() : "";

    int64_t ts = 0;
    const auto& tsv = payload["ts_unix_ms"];
    if (tsv.isInt64()) ts = tsv.asInt64();
    else if (tsv.isUInt64()) ts = (int64_t)tsv.asUInt64();
    const std::string et = payload.isMember("type") && payload["type"].isString() ? payload["type"].asString() : "";

    const std::string key = cid + "\n" + inst + "\n" + kind;
    auto it = seen.find(key);
    if (it == seen.end() || ts >= it->second.ts_unix_ms) {
      SeenClient sc;
      sc.id = cid;
      sc.kind = kind;
      sc.instance_id = inst;
      sc.ts_unix_ms = ts;
      sc.last_type = et;
      sc.last_event = payload;
      seen[key] = std::move(sc);
    }
  }

  std::vector<SeenClient> clients;
  clients.reserve(seen.size());
  for (auto& kv : seen) {
    clients.push_back(std::move(kv.second));
  }
  std::sort(clients.begin(), clients.end(), [](const SeenClient& a, const SeenClient& b) {
    return a.ts_unix_ms > b.ts_unix_ms;
  });

  Json::Value arr(Json::arrayValue);
  for (auto& c : clients) {
    Json::Value v(Json::objectValue);
    v["id"] = c.id;
    if (!c.kind.empty()) v["kind"] = c.kind;
    if (!c.instance_id.empty()) v["instance_id"] = c.instance_id;
    v["last_ts_unix_ms"] = (Json::Int64)c.ts_unix_ms;
    if (!c.last_type.empty()) v["last_type"] = c.last_type;

    if (include_data && c.last_event.isObject()) {
      // Include a bounded view of the last event data to support “polling” style reasoning.
      Json::Value le = c.last_event;
      const Json::Value data = le["data"];
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      const std::string data_json = Json::writeString(wb, data);
      if (data_json.size() <= max_data_bytes) {
        v["last_event"] = le;
      } else {
        Json::Value s(Json::objectValue);
        s["truncated"] = true;
        s["bytes"] = (Json::UInt64)data_json.size();
        s["summary"] = summarize_data(data, 32);
        le["data"] = s;
        v["last_event"] = le;
      }
    }

    arr.append(v);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value d(Json::objectValue);
  d["tool"] = "client_peek";
  d["session_id"] = ctx->session_id;
  d["max_bytes"] = (Json::UInt64)max_bytes;
  d["max_files"] = (Json::UInt64)max_files;
  if (!only_client_id.empty()) d["client_id"] = only_client_id;
  d["count"] = (Json::UInt64)arr.size();
  d["clients"] = arr;
  o["data"] = d;
  return write_result(out_result, o);
}

#endif

} // namespace host_tools_internal

