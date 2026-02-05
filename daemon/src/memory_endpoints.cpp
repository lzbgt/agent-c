#include "memory_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "memory_checkpoints.h"
#include "memory_consolidator.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agentd {

namespace {
static std::filesystem::path memory_root_from_cfg(const DaemonConfig& cfg) {
  if (cfg.state_dir.empty()) return {};
  return (std::filesystem::path(cfg.state_dir) / "memory").lexically_normal();
}

}  // namespace

void handle_memory_consolidate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args(Json::objectValue);
  if (!req.body.empty()) {
    std::string perr;
    if (!json_parse_object(req.body, &args, &perr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string("invalid JSON: ") + perr;
      resp->body = json_stringify(o);
      return;
    }
  }

  MemoryConsolidateOptions opt;
  opt.daily_days = cfg.memory_consolidate_daily_days;
  opt.keep_checkpoints = cfg.memory_consolidate_keep_checkpoints;

  if (args.isMember("daily_days") && args["daily_days"].isInt()) {
    opt.daily_days = std::max(0, args["daily_days"].asInt());
  }
  if (args.isMember("keep_checkpoints") && args["keep_checkpoints"].isInt()) {
    opt.keep_checkpoints = std::max(1, args["keep_checkpoints"].asInt());
  }
  if (args.isMember("max_entries") && args["max_entries"].isInt()) {
    opt.max_entries = std::max(1, args["max_entries"].asInt());
  }
  if (args.isMember("dry_run") && args["dry_run"].isBool()) {
    opt.dry_run = args["dry_run"].asBool();
  }

  Json::Value report;
  std::string err;
  if (!memory_consolidate_once(cfg, opt, &report, &err)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err.empty() ? "memory consolidation failed" : err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["data"] = report;
  resp->body = json_stringify(o);
  return;
}

void handle_memory_checkpoints_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  const std::filesystem::path ckdir = mem_root / "checkpoints";
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(ckdir, ec) || !std::filesystem::is_directory(ckdir, ec)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints directory\"}";
    return;
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (const auto v = query_get(req.query, "since_utc_ms"); v && !v->empty()) {
    try { since_ms = (int64_t)std::stoll(*v); } catch (...) { since_ms = 0; }
  }
  if (const auto v = query_get(req.query, "until_utc_ms"); v && !v->empty()) {
    try { until_ms = (int64_t)std::stoll(*v); } catch (...) { until_ms = INT64_MAX; }
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);

  const auto structured_path_q = query_get(req.query, "structured_path");
  const std::string structured_path_filter =
    structured_path_q && !structured_path_q->empty() ? *structured_path_q : "";

  int limit = 50;
  if (const auto v = query_get(req.query, "limit"); v && !v->empty()) {
    try { limit = (int)std::stol(*v); } catch (...) { limit = 50; }
  }
  limit = std::max(1, std::min(200, limit));
  std::vector<MemoryCheckpointMeta> rows;
  std::string lerr;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, limit, &rows, &lerr)) {
    resp->status = 404;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = lerr.empty() ? "no checkpoints in window" : lerr;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["memory_root"] = mem_root.generic_string();
  o["since_utc_ms"] = (Json::Int64)since_ms;
  o["until_utc_ms"] = (Json::Int64)until_ms;
  if (!structured_path_filter.empty()) o["structured_path_filter"] = structured_path_filter;
  Json::Value arr(Json::arrayValue);
  for (const auto& r : rows) {
    Json::Value row(Json::objectValue);
    row["checkpoint_path"] = r.checkpoint_path_rel;
    row["structured_path"] = r.structured_path;
    row["ts_utc"] = r.ts_utc;
    row["ts_utc_ms"] = (Json::Int64)r.ts_utc_ms;
    row["sha256"] = r.sha256;
    row["bytes"] = (Json::Int64)r.bytes;
    arr.append(row);
  }
  o["checkpoints"] = arr;
  resp->body = json_stringify(o);
}

void handle_memory_correlate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto trace_id = query_get(req.query, "trace_id");
  const std::string tid = trace_id && !trace_id->empty() ? *trace_id : "";
  if (tid.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing trace_id\"}";
    return;
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (const auto v = query_get(req.query, "since_utc_ms"); v && !v->empty()) {
    try { since_ms = (int64_t)std::stoll(*v); } catch (...) { since_ms = 0; }
  }
  if (const auto v = query_get(req.query, "until_utc_ms"); v && !v->empty()) {
    try { until_ms = (int64_t)std::stoll(*v); } catch (...) { until_ms = INT64_MAX; }
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);

  const auto structured_path_q = query_get(req.query, "structured_path");
  const std::string structured_path_filter =
    structured_path_q && !structured_path_q->empty() ? *structured_path_q : "";

  const auto key_prefix_q = query_get(req.query, "key_prefix");
  const std::string key_prefix =
    key_prefix_q && !key_prefix_q->empty() ? *key_prefix_q : "";

  int max_entries = 50;
  if (const auto v = query_get(req.query, "max_entries"); v && !v->empty()) {
    try { max_entries = (int)std::stol(*v); } catch (...) { max_entries = 50; }
  }
  max_entries = std::max(1, std::min(500, max_entries));

  bool timeline = false;
  if (const auto v = query_get(req.query, "timeline"); v) {
    if (v->empty()) {
      timeline = true;
    } else {
      std::string s = *v;
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      timeline = (s == "1" || s == "true" || s == "yes");
    }
  }

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  const std::string needle = std::string("trace:") + tid;

  const int ck_limit = timeline ? 50 : 1;
  std::vector<MemoryCheckpointMeta> metas;
  std::string lerr;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, ck_limit, &metas, &lerr) || metas.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints in window\"}";
    return;
  }

  auto extract_entries = [&](const MemoryCheckpointMeta& meta, Json::Value* out_entries, Json::Value* out_ckinfo) -> bool {
    if (!out_entries || !out_ckinfo) return false;
    *out_entries = Json::Value(Json::arrayValue);
    *out_ckinfo = Json::Value(Json::objectValue);
    if (meta.checkpoint_path_rel.empty()) return false;

    std::string structured_path2;
    Json::Value items(Json::nullValue);
    std::string rerr;
    if (!memory_read_structured_checkpoint_items(mem_root, meta.checkpoint_path_rel, &structured_path2, &items, &rerr)) return false;

    (*out_ckinfo)["checkpoint_path"] = meta.checkpoint_path_rel;
    (*out_ckinfo)["structured_path"] = meta.structured_path;
    (*out_ckinfo)["ts_utc"] = meta.ts_utc;
    (*out_ckinfo)["ts_utc_ms"] = (Json::Int64)meta.ts_utc_ms;
    (*out_ckinfo)["sha256"] = meta.sha256;
    (*out_ckinfo)["bytes"] = (Json::Int64)meta.bytes;

    if (!items.isObject()) return false;
    std::vector<std::string> keys = items.getMemberNames();
    std::sort(keys.begin(), keys.end());

    int added = 0;
    for (const auto& key : keys) {
      if (added >= max_entries) break;
      if (!key_prefix.empty() && key.rfind(key_prefix, 0) != 0) continue;
      const Json::Value rec = items[key];
      if (!rec.isObject()) continue;
      const Json::Value sources = rec.isMember("sources") ? rec["sources"] : Json::Value(Json::nullValue);
      if (!sources.isArray()) continue;
      bool hit = false;
      for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
        if (!sources[i].isString()) continue;
        const std::string s = sources[i].asString();
        if (s.find(needle) != std::string::npos) { hit = true; break; }
      }
      if (!hit) continue;
      Json::Value row(Json::objectValue);
      row["key"] = key;
      row["record"] = rec;
      (*out_entries).append(row);
      added++;
    }
    return true;
  };

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["trace_id"] = tid;
  out["needle"] = needle;
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  if (!structured_path_filter.empty()) out["structured_path_filter"] = structured_path_filter;
  if (!key_prefix.empty()) out["key_prefix"] = key_prefix;

  if (!timeline) {
    const MemoryCheckpointMeta& newest = metas[0];
    Json::Value entries, ckinfo;
    if (!extract_entries(newest, &entries, &ckinfo)) {
      resp->status = 500;
      resp->body = "{\"ok\":false,\"error\":\"failed to extract entries from newest checkpoint\"}";
      return;
    }
    out["checkpoint"] = ckinfo;
    out["entries"] = entries;
  } else {
    Json::Value timeline_arr(Json::arrayValue);
    for (size_t i = 0; i < metas.size(); i++) {
      Json::Value entries, ckinfo;
      if (!extract_entries(metas[i], &entries, &ckinfo)) continue;
      Json::Value row(Json::objectValue);
      row["checkpoint"] = ckinfo;
      row["entries"] = entries;
      timeline_arr.append(row);
      if ((int)timeline_arr.size() >= 50) break;
    }
    out["timeline"] = timeline_arr;
  }

  resp->body = json_stringify(out);
}

void handle_memory_query_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (const auto v = query_get(req.query, "since_utc_ms"); v && !v->empty()) {
    try { since_ms = (int64_t)std::stoll(*v); } catch (...) { since_ms = 0; }
  }
  if (const auto v = query_get(req.query, "until_utc_ms"); v && !v->empty()) {
    try { until_ms = (int64_t)std::stoll(*v); } catch (...) { until_ms = INT64_MAX; }
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);

  const auto structured_path_q = query_get(req.query, "structured_path");
  const std::string structured_path_filter =
    structured_path_q && !structured_path_q->empty() ? *structured_path_q : "";

  const auto key_prefix_q = query_get(req.query, "key_prefix");
  const std::string key_prefix =
    key_prefix_q && !key_prefix_q->empty() ? *key_prefix_q : "";

  int limit = 50;
  if (const auto v = query_get(req.query, "limit"); v && !v->empty()) {
    try { limit = (int)std::stol(*v); } catch (...) { limit = 50; }
  }
  limit = std::max(1, std::min(1000, limit));

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  if (mem_root.empty()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"missing memory root\"}";
    return;
  }
  std::vector<MemoryCheckpointMeta> metas;
  std::string lerr;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, /*limit=*/1, &metas, &lerr) || metas.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints in window\"}";
    return;
  }
  const MemoryCheckpointMeta& newest = metas[0];

  std::string structured_path2;
  Json::Value items(Json::nullValue);
  std::string rerr;
  if (!memory_read_structured_checkpoint_items(mem_root, newest.checkpoint_path_rel, &structured_path2, &items, &rerr)) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"failed to read checkpoint\"}";
    return;
  }
  if (!items.isObject()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"checkpoint missing doc.items\"}";
    return;
  }

  std::vector<std::string> keys = items.getMemberNames();
  std::sort(keys.begin(), keys.end());

  Json::Value entries(Json::arrayValue);
  int returned = 0;
  for (const auto& key : keys) {
    if (returned >= limit) break;
    if (!key_prefix.empty() && key.rfind(key_prefix, 0) != 0) continue;
    const Json::Value rec = items[key];
    if (!rec.isObject()) continue;
    Json::Value row(Json::objectValue);
    row["key"] = key;
    row["record"] = rec;
    entries.append(row);
    returned++;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["memory_root"] = mem_root.generic_string();
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  if (!structured_path_filter.empty()) out["structured_path_filter"] = structured_path_filter;
  if (!key_prefix.empty()) out["key_prefix"] = key_prefix;
  out["limit"] = limit;
  {
    Json::Value ckinfo(Json::objectValue);
    ckinfo["checkpoint_path"] = newest.checkpoint_path_rel;
    ckinfo["structured_path"] = newest.structured_path;
    ckinfo["ts_utc"] = newest.ts_utc;
    ckinfo["ts_utc_ms"] = (Json::Int64)newest.ts_utc_ms;
    ckinfo["sha256"] = newest.sha256;
    ckinfo["bytes"] = (Json::Int64)newest.bytes;
    out["checkpoint"] = ckinfo;
  }
  out["entries"] = entries;
  out["returned"] = returned;
  resp->body = json_stringify(out);
}

}  // namespace agentd
