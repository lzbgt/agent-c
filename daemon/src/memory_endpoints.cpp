#include "memory_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "memory_checkpoints.h"
#include "memory_correlation_index.h"
#include "memory_correlation_graph.h"
#include "memory_consolidator.h"
#include "memory_recaps.h"
#include "memory_retention.h"
#include "memory_salience.h"
#include "session_id_util.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace agentd {

namespace {
static std::filesystem::path memory_root_from_cfg(const DaemonConfig& cfg) {
  if (cfg.state_dir.empty()) return {};
  return (std::filesystem::path(cfg.state_dir) / "memory").lexically_normal();
}

static std::string local_date_ymd_days_ago(int days_ago) {
  const auto now = std::chrono::system_clock::now() - std::chrono::hours(24 * std::max(0, days_ago));
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

static bool count_lines_capped(const std::filesystem::path& p, int64_t* out_lines) {
  if (!out_lines) return false;
  *out_lines = 0;
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::string line;
  while (std::getline(in, line)) {
    (*out_lines)++;
    if (*out_lines > 1000000) break;
  }
  return true;
}

struct MemoryIndexRow {
  std::string tier;
  std::string rel_path;
  int64_t bytes = 0;
  int64_t lines = 0;
  int64_t token_estimate = 0;
};

static double parse_double_or(const std::optional<std::string>& v, double fallback) {
  if (!v || v->empty()) return fallback;
  try {
    return std::stod(*v);
  } catch (...) {
    return fallback;
  }
}

static std::string path_rel_to(const std::filesystem::path& root, const std::filesystem::path& abs) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
  if (ec) rel = abs.lexically_relative(root);
  return rel.generic_string();
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
  if (!opt.dry_run) {
    MemoryCorrelationIndexReport idx_rep;
    std::string idx_err;
    MemoryCorrelationIndexOptions idx_opt = memory_correlation_index_default_options();
    if (memory_correlation_index_build(memory_root_from_cfg(cfg), idx_opt, &idx_rep, &idx_err)) {
      Json::Value ix(Json::objectValue);
      ix["ok"] = true;
      ix["generated_utc_ms"] = (Json::Int64)idx_rep.generated_utc_ms;
      if (!idx_rep.generated_utc.empty()) ix["generated_utc"] = idx_rep.generated_utc;
      ix["index_path"] = idx_rep.index_path;
      ix["token_count"] = (Json::Int64)idx_rep.token_count;
      ix["entry_count"] = (Json::Int64)idx_rep.entry_count;
      o["correlation_index"] = ix;
    } else if (!idx_err.empty()) {
      Json::Value ix(Json::objectValue);
      ix["ok"] = false;
      ix["error"] = idx_err;
      o["correlation_index"] = ix;
    }
  }
  resp->body = json_stringify(o);
  return;
}

void handle_memory_retention_endpoint(
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

  MemoryRetentionPolicy policy;
  policy.daily_max_days = cfg.memory_retention_daily_max_days;
  policy.daily_max_bytes = cfg.memory_retention_daily_max_bytes;
  policy.checkpoint_max_days = cfg.memory_retention_checkpoint_max_days;
  policy.checkpoint_max_count = cfg.memory_retention_checkpoint_max_count;
  policy.structured_deprecate_days = cfg.memory_retention_structured_deprecate_days;
  policy.structured_deprecate_max_entries = cfg.memory_retention_structured_deprecate_max_entries;

  if (args.isMember("dry_run") && args["dry_run"].isBool()) {
    policy.dry_run = args["dry_run"].asBool();
  }
  if (args.isMember("daily_max_days") && (args["daily_max_days"].isInt() || args["daily_max_days"].isUInt())) {
    const int n = args["daily_max_days"].isInt() ? args["daily_max_days"].asInt() : (int)args["daily_max_days"].asUInt();
    policy.daily_max_days = std::max(0, n);
  }
  if (args.isMember("daily_max_bytes") && (args["daily_max_bytes"].isInt64() || args["daily_max_bytes"].isUInt64())) {
    const int64_t n = args["daily_max_bytes"].isInt64()
      ? args["daily_max_bytes"].asInt64()
      : (int64_t)args["daily_max_bytes"].asUInt64();
    policy.daily_max_bytes = std::max<int64_t>(0, n);
  }
  if (args.isMember("checkpoint_max_days") && (args["checkpoint_max_days"].isInt() || args["checkpoint_max_days"].isUInt())) {
    const int n = args["checkpoint_max_days"].isInt()
      ? args["checkpoint_max_days"].asInt()
      : (int)args["checkpoint_max_days"].asUInt();
    policy.checkpoint_max_days = std::max(0, n);
  }
  if (args.isMember("checkpoint_max_count") && (args["checkpoint_max_count"].isInt() || args["checkpoint_max_count"].isUInt())) {
    const int n = args["checkpoint_max_count"].isInt()
      ? args["checkpoint_max_count"].asInt()
      : (int)args["checkpoint_max_count"].asUInt();
    policy.checkpoint_max_count = std::max(0, n);
  }
  if (args.isMember("structured_deprecate_days") &&
      (args["structured_deprecate_days"].isInt() || args["structured_deprecate_days"].isUInt())) {
    const int n = args["structured_deprecate_days"].isInt()
      ? args["structured_deprecate_days"].asInt()
      : (int)args["structured_deprecate_days"].asUInt();
    policy.structured_deprecate_days = std::max(0, n);
  }
  if (args.isMember("structured_deprecate_max_entries") &&
      (args["structured_deprecate_max_entries"].isInt() || args["structured_deprecate_max_entries"].isUInt())) {
    const int n = args["structured_deprecate_max_entries"].isInt()
      ? args["structured_deprecate_max_entries"].asInt()
      : (int)args["structured_deprecate_max_entries"].asUInt();
    policy.structured_deprecate_max_entries = std::max(0, n);
  }

  MemoryRetentionStats stats;
  std::string err;
  if (!memory_retention_enforce(cfg, policy, &stats, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err.empty() ? "memory retention failed" : err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["generated_utc_ms"] = (Json::Int64)stats.generated_utc_ms;
  o["dry_run"] = policy.dry_run;
  o["daily_max_days"] = policy.daily_max_days;
  o["daily_max_bytes"] = (Json::Int64)policy.daily_max_bytes;
  o["checkpoint_max_days"] = policy.checkpoint_max_days;
  o["checkpoint_max_count"] = policy.checkpoint_max_count;
  o["structured_deprecate_days"] = policy.structured_deprecate_days;
  o["structured_deprecate_max_entries"] = policy.structured_deprecate_max_entries;
  o["daily_deleted_count"] = (Json::Int64)stats.daily_deleted_count;
  o["checkpoint_deleted_count"] = (Json::Int64)stats.checkpoint_deleted_count;
  o["structured_deprecated_count"] = (Json::Int64)stats.structured_deprecated_count;
  o["daily_bytes_before"] = (Json::Int64)stats.daily_bytes_before;
  o["daily_bytes_after"] = (Json::Int64)stats.daily_bytes_after;
  if (!stats.daily_deleted.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : stats.daily_deleted) arr.append(p);
    o["daily_deleted"] = arr;
  }
  if (!stats.checkpoint_deleted.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : stats.checkpoint_deleted) arr.append(p);
    o["checkpoint_deleted"] = arr;
  }
  if (!stats.structured_deprecated_keys.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& k : stats.structured_deprecated_keys) arr.append(k);
    o["structured_deprecated_keys"] = arr;
  }
  if (!stats.errors.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& e : stats.errors) arr.append(e);
    o["errors"] = arr;
  }
  resp->body = json_stringify(o);
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
    resp->body = json_error_body("no checkpoints directory");
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
    resp->body = json_error_body("missing trace_id");
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
    bool used_index = false;
    Json::Value idx_entries(Json::arrayValue);
    Json::Value idx_meta(Json::objectValue);
    std::string idx_err;
    const int idx_limit = std::min(2000, max_entries * 3);
    if (memory_correlation_index_query(mem_root, needle, idx_limit, &idx_entries, &idx_meta, &idx_err)) {
      used_index = true;
      idx_meta["ok"] = true;
      idx_meta["token"] = needle;
      out["index"] = idx_meta;

      Json::Value structured_entries(Json::arrayValue);
      Json::Value daily_entries(Json::arrayValue);
      Json::Value recap_entries(Json::arrayValue);

      int structured_added = 0;
      int daily_added = 0;
      int recap_added = 0;

      for (Json::ArrayIndex i = 0; i < idx_entries.size(); i++) {
        const Json::Value e = idx_entries[i];
        if (!e.isObject()) continue;
        const std::string kind = e.isMember("kind") && e["kind"].isString() ? e["kind"].asString() : "";
        if (kind == "structured") {
          if (structured_added >= max_entries) continue;
          const std::string key = e.isMember("key") && e["key"].isString() ? e["key"].asString() : "";
          if (key.empty()) continue;
          if (!key_prefix.empty() && key.rfind(key_prefix, 0) != 0) continue;
          if (!structured_path_filter.empty()) {
            const std::string sp = e.isMember("structured_path") && e["structured_path"].isString() ? e["structured_path"].asString() : "";
            if (sp != structured_path_filter) continue;
          }
          Json::Value row(Json::objectValue);
          row["key"] = key;
          if (e.isMember("record")) row["record"] = e["record"];
          if (e.isMember("structured_path")) row["structured_path"] = e["structured_path"];
          if (e.isMember("checkpoint_path")) row["checkpoint_path"] = e["checkpoint_path"];
          if (e.isMember("checkpoint_ts_utc")) row["checkpoint_ts_utc"] = e["checkpoint_ts_utc"];
          if (e.isMember("checkpoint_ts_utc_ms")) row["checkpoint_ts_utc_ms"] = e["checkpoint_ts_utc_ms"];
          structured_entries.append(row);
          structured_added++;
        } else if (kind == "daily") {
          if (daily_added >= max_entries) continue;
          daily_entries.append(e);
          daily_added++;
        } else if (kind == "recap") {
          if (recap_added >= max_entries) continue;
          recap_entries.append(e);
          recap_added++;
        }
      }

      out["entries"] = structured_entries;
      if (daily_entries.size() > 0) out["daily_entries"] = daily_entries;
      if (recap_entries.size() > 0) out["recap_entries"] = recap_entries;
    }

    if (!used_index) {
      if (!idx_err.empty()) {
        Json::Value ix(Json::objectValue);
        ix["ok"] = false;
        ix["error"] = idx_err;
        out["index"] = ix;
      }

      const int ck_limit = 1;
      std::vector<MemoryCheckpointMeta> metas;
      std::string lerr;
      if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, ck_limit, &metas, &lerr) || metas.empty()) {
        resp->status = 404;
        resp->body = json_error_body("no checkpoints in window");
        return;
      }

      const MemoryCheckpointMeta& newest = metas[0];
      Json::Value entries, ckinfo;
      if (!extract_entries(newest, &entries, &ckinfo)) {
        resp->status = 500;
        resp->body = json_error_body("failed to extract entries from newest checkpoint");
        return;
      }
      out["checkpoint"] = ckinfo;
      out["entries"] = entries;
    }
  } else {
    const int ck_limit = 50;
    std::vector<MemoryCheckpointMeta> metas;
    std::string lerr;
    if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, ck_limit, &metas, &lerr) || metas.empty()) {
      resp->status = 404;
      resp->body = json_error_body("no checkpoints in window");
      return;
    }
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

  out["relationship_graph"] = memory_correlation_relationship_graph_from_response(tid, out);
  resp->body = json_stringify(out);
}

void handle_memory_correlation_index_build_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  if (mem_root.empty()) {
    resp->status = 400;
    resp->body = json_error_body("memory root not configured");
    return;
  }

  MemoryCorrelationIndexOptions opt = memory_correlation_index_default_options();

  if (!req.body.empty()) {
    Json::Value args;
    std::string perr;
    if (!json_parse_any(req.body, &args, &perr) || !args.isObject()) {
      resp->status = 400;
      resp->body = json_error_body("invalid JSON body");
      return;
    }

    if (args.isMember("daily_days") && args["daily_days"].isInt()) {
      opt.daily_days = std::max(0, std::min(365, args["daily_days"].asInt()));
    }
    if (args.isMember("max_daily_entries") && args["max_daily_entries"].isInt()) {
      opt.max_daily_entries = std::max(1, std::min(1000, args["max_daily_entries"].asInt()));
    }
    if (args.isMember("max_structured_checkpoints") && args["max_structured_checkpoints"].isInt()) {
      opt.max_structured_checkpoints = std::max(1, std::min(200, args["max_structured_checkpoints"].asInt()));
    }
    if (args.isMember("max_structured_entries") && args["max_structured_entries"].isInt()) {
      opt.max_structured_entries = std::max(1, std::min(2000, args["max_structured_entries"].asInt()));
    }
    if (args.isMember("max_entries_per_token") && args["max_entries_per_token"].isInt()) {
      opt.max_entries_per_token = std::max(1, std::min(2000, args["max_entries_per_token"].asInt()));
    }
    if (args.isMember("max_recaps") && args["max_recaps"].isInt()) {
      opt.max_recaps = std::max(1, std::min(1000, args["max_recaps"].asInt()));
    }
    if (args.isMember("value_excerpt_chars") && args["value_excerpt_chars"].isInt()) {
      opt.value_excerpt_chars = (size_t)std::max(32, std::min(2000, args["value_excerpt_chars"].asInt()));
    }
    if (args.isMember("include_structured") && args["include_structured"].isBool()) {
      opt.include_structured = args["include_structured"].asBool();
    }
    if (args.isMember("include_daily") && args["include_daily"].isBool()) {
      opt.include_daily = args["include_daily"].asBool();
    }
    if (args.isMember("include_recaps") && args["include_recaps"].isBool()) {
      opt.include_recaps = args["include_recaps"].asBool();
    }
  }

  MemoryCorrelationIndexReport rep;
  std::string err;
  if (!memory_correlation_index_build(mem_root, opt, &rep, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err.empty() ? "failed to build correlation index" : err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["memory_root"] = mem_root.string();
  o["generated_utc_ms"] = (Json::Int64)rep.generated_utc_ms;
  if (!rep.generated_utc.empty()) o["generated_utc"] = rep.generated_utc;
  o["index_path"] = rep.index_path;
  o["token_count"] = (Json::Int64)rep.token_count;
  o["entry_count"] = (Json::Int64)rep.entry_count;
  o["structured_entries"] = (Json::Int64)rep.structured_entries;
  o["daily_entries"] = (Json::Int64)rep.daily_entries;
  o["recap_entries"] = (Json::Int64)rep.recap_entries;
  resp->body = json_stringify(o);
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
    resp->body = json_error_body("missing memory root");
    return;
  }
  std::vector<MemoryCheckpointMeta> metas;
  std::string lerr;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, /*limit=*/1, &metas, &lerr) || metas.empty()) {
    resp->status = 404;
    resp->body = json_error_body("no checkpoints in window");
    return;
  }
  const MemoryCheckpointMeta& newest = metas[0];

  std::string structured_path2;
  Json::Value items(Json::nullValue);
  std::string rerr;
  if (!memory_read_structured_checkpoint_items(mem_root, newest.checkpoint_path_rel, &structured_path2, &items, &rerr)) {
    resp->status = 500;
    resp->body = json_error_body("failed to read checkpoint");
    return;
  }
  if (!items.isObject()) {
    resp->status = 500;
    resp->body = json_error_body("checkpoint missing doc.items");
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

void handle_memory_index_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    resp->status = 404;
    resp->body = json_error_body("memory root not found");
    return;
  }

  const auto session_q = query_get(req.query, "session_id");
  const std::string session_id = session_q && !session_q->empty() ? *session_q : "";
  if (!session_id.empty() && !session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  const auto include_structured_q = query_get(req.query, "include_structured");
  const auto include_core_q = query_get(req.query, "include_core");
  const auto include_daily_q = query_get(req.query, "include_daily");
  const auto include_session_q = query_get(req.query, "include_session");
  bool include_structured = include_structured_q ? string_to_bool(*include_structured_q) : true;
  bool include_core = include_core_q ? string_to_bool(*include_core_q) : true;
  bool include_daily = include_daily_q ? string_to_bool(*include_daily_q) : true;
  bool include_session = include_session_q ? string_to_bool(*include_session_q) : true;

  int daily_days = 2;
  if (const auto v = query_get(req.query, "daily_days"); v && !v->empty()) {
    try { daily_days = (int)std::stol(*v); } catch (...) { daily_days = 2; }
  }
  daily_days = std::max(0, std::min(31, daily_days));

  std::vector<MemoryIndexRow> rows;
  rows.reserve(8 + (size_t)daily_days);

  auto push_row = [&](const std::string& tier, const std::filesystem::path& p) {
    ec.clear();
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) return;
    MemoryIndexRow row;
    row.tier = tier;
    row.rel_path = path_rel_to(mem_root, p);
    row.bytes = (int64_t)std::filesystem::file_size(p, ec);
    if (ec) row.bytes = 0;
    (void)count_lines_capped(p, &row.lines);
    row.token_estimate = row.bytes > 0 ? (row.bytes + 3) / 4 : 0;
    rows.push_back(std::move(row));
  };

  if (include_structured) push_row("structured", mem_root / "STRUCTURED.md");
  if (include_core) push_row("core", mem_root / "MEMORY.md");
  if (include_daily) {
    for (int i = 0; i < daily_days; i++) {
      const std::string ymd = local_date_ymd_days_ago(i);
      push_row("daily", mem_root / (ymd + ".md"));
    }
  }
  if (include_session && !session_id.empty()) {
    push_row("session", mem_root / "sessions" / (session_id + ".md"));
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["memory_root"] = mem_root.generic_string();
  if (!session_id.empty()) o["session_id"] = session_id;
  o["daily_days"] = daily_days;
  o["include_structured"] = include_structured;
  o["include_core"] = include_core;
  o["include_daily"] = include_daily;
  o["include_session"] = include_session;

  Json::Value arr(Json::arrayValue);
  int64_t total_bytes = 0;
  int64_t total_tokens = 0;
  for (const auto& r : rows) {
    Json::Value row(Json::objectValue);
    row["tier"] = r.tier;
    row["path"] = r.rel_path;
    row["bytes"] = (Json::Int64)r.bytes;
    row["lines"] = (Json::Int64)r.lines;
    row["token_estimate"] = (Json::Int64)r.token_estimate;
    total_bytes += std::max<int64_t>(0, r.bytes);
    total_tokens += std::max<int64_t>(0, r.token_estimate);
    arr.append(row);
  }
  o["files"] = arr;
  o["total_bytes"] = (Json::Int64)total_bytes;
  o["total_token_estimate"] = (Json::Int64)total_tokens;
  resp->body = json_stringify(o);
}

void handle_memory_salience_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    resp->status = 404;
    resp->body = json_error_body("memory root not found");
    return;
  }

  MemorySaliencePolicy pol;
  pol.include_structured = true;
  pol.include_daily = true;
  pol.daily_days = cfg.memory_salience_daily_days;
  pol.max_items = cfg.memory_salience_max_items;
  pol.max_structured_items = cfg.memory_salience_structured_max_items;
  pol.max_daily_items = cfg.memory_salience_daily_max_items;
  pol.half_life_days = cfg.memory_salience_half_life_days;
  pol.importance_weight = cfg.memory_salience_importance_weight;

  if (const auto v = query_get(req.query, "include_structured"); v && !v->empty()) {
    pol.include_structured = string_to_bool(*v);
  }
  if (const auto v = query_get(req.query, "include_daily"); v && !v->empty()) {
    pol.include_daily = string_to_bool(*v);
  }
  if (const auto v = query_get(req.query, "daily_days"); v && !v->empty()) {
    try { pol.daily_days = (int)std::stol(*v); } catch (...) {}
  }
  if (const auto v = query_get(req.query, "max_items"); v && !v->empty()) {
    try { pol.max_items = (int)std::stol(*v); } catch (...) {}
  }
  if (const auto v = query_get(req.query, "max_structured_items"); v && !v->empty()) {
    try { pol.max_structured_items = (int)std::stol(*v); } catch (...) {}
  }
  if (const auto v = query_get(req.query, "max_daily_items"); v && !v->empty()) {
    try { pol.max_daily_items = (int)std::stol(*v); } catch (...) {}
  }
  pol.half_life_days = parse_double_or(query_get(req.query, "half_life_days"), pol.half_life_days);
  pol.importance_weight = parse_double_or(query_get(req.query, "importance_weight"), pol.importance_weight);

  pol.daily_days = std::max(0, std::min(31, pol.daily_days));
  pol.max_items = std::max(1, std::min(200, pol.max_items));
  pol.max_structured_items = std::max(0, std::min(200, pol.max_structured_items));
  pol.max_daily_items = std::max(0, std::min(200, pol.max_daily_items));
  if (pol.half_life_days < 0) pol.half_life_days = 0;
  if (pol.importance_weight < 0) pol.importance_weight = 0;

  MemorySalienceReport rep;
  std::string err;
  if (!memory_salience_collect(mem_root, pol, &rep, &err)) {
    resp->status = 500;
    resp->body = json_error_body("failed to compute memory salience");
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["memory_root"] = mem_root.generic_string();
  out["generated_utc_ms"] = (Json::Int64)rep.generated_utc_ms;
  {
    Json::Value p(Json::objectValue);
    p["include_structured"] = pol.include_structured;
    p["include_daily"] = pol.include_daily;
    p["daily_days"] = pol.daily_days;
    p["max_items"] = pol.max_items;
    p["max_structured_items"] = pol.max_structured_items;
    p["max_daily_items"] = pol.max_daily_items;
    p["half_life_days"] = pol.half_life_days;
    p["importance_weight"] = pol.importance_weight;
    out["policy"] = p;
  }
  if (rep.structured_checkpoint_found) {
    Json::Value ck(Json::objectValue);
    ck["checkpoint_path"] = rep.structured_checkpoint.checkpoint_path_rel;
    ck["structured_path"] = rep.structured_checkpoint.structured_path;
    ck["ts_utc"] = rep.structured_checkpoint.ts_utc;
    ck["ts_utc_ms"] = (Json::Int64)rep.structured_checkpoint.ts_utc_ms;
    ck["sha256"] = rep.structured_checkpoint.sha256;
    ck["bytes"] = (Json::Int64)rep.structured_checkpoint.bytes;
    out["structured_checkpoint"] = ck;
  }

  Json::Value structured(Json::arrayValue);
  for (const auto& item : rep.structured_items) {
    Json::Value row(Json::objectValue);
    row["key"] = item.key;
    if (!item.kind.empty()) row["kind"] = item.kind;
    if (!item.status.empty()) row["status"] = item.status;
    if (!item.ts_utc.empty()) row["updated_utc"] = item.ts_utc;
    row["value"] = item.text;
    row["score"] = item.score;
    structured.append(row);
  }
  Json::Value daily(Json::arrayValue);
  for (const auto& item : rep.daily_items) {
    Json::Value row(Json::objectValue);
    row["path"] = item.path;
    row["line"] = item.line;
    row["text"] = item.text;
    row["score"] = item.score;
    if (!item.ts_utc.empty()) row["ts_utc"] = item.ts_utc;
    if (item.importance >= 0) row["importance"] = item.importance;
    daily.append(row);
  }
  out["structured_items"] = structured;
  out["daily_items"] = daily;
  out["returned"] = (Json::Int64)(rep.structured_items.size() + rep.daily_items.size());
  if (!rep.errors.empty()) {
    Json::Value errs(Json::arrayValue);
    for (const auto& e : rep.errors) errs.append(e);
    out["errors"] = errs;
  }
  resp->body = json_stringify(out);
}

void handle_memory_recaps_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (req.method == "GET") {
    int limit = 20;
    std::string kind;
    if (const auto v = query_get(req.query, "limit"); v && !v->empty()) {
      try { limit = (int)std::stol(*v); } catch (...) { limit = 20; }
    }
    if (const auto v = query_get(req.query, "kind"); v && !v->empty()) {
      kind = *v;
    }
    limit = std::max(1, std::min(200, limit));
    bool include_summary = false;
    if (const auto v = query_get(req.query, "include_summary"); v && !v->empty()) {
      include_summary = string_to_bool(*v);
    }

    Json::Value recaps(Json::arrayValue);
    std::string err;
    if (!memory_list_recaps(cfg, limit, include_summary, kind, &recaps, &err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = err.empty() ? "failed to list recaps" : err;
      resp->body = json_stringify(o);
      return;
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["memory_root"] = memory_root_from_cfg(cfg).generic_string();
    o["limit"] = limit;
    o["include_summary"] = include_summary;
    o["recaps"] = recaps;
    o["count"] = (Json::Int64)recaps.size();
    resp->body = json_stringify(o);
    return;
  }

  if (req.method != "POST") {
    resp->status = 405;
    resp->body = json_error_body("method not allowed");
    return;
  }

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

  MemoryRecapOptions opt;
  opt.salience.include_structured = true;
  opt.salience.include_daily = true;
  opt.salience.daily_days = cfg.memory_salience_daily_days;
  opt.salience.max_items = cfg.memory_salience_max_items;
  opt.salience.max_structured_items = cfg.memory_salience_structured_max_items;
  opt.salience.max_daily_items = cfg.memory_salience_daily_max_items;
  opt.salience.half_life_days = cfg.memory_salience_half_life_days;
  opt.salience.importance_weight = cfg.memory_salience_importance_weight;
  opt.summary_max_chars = cfg.summary_max_chars;
  opt.model = cfg.summary_model;

  if (args.isMember("dry_run") && args["dry_run"].isBool()) opt.dry_run = args["dry_run"].asBool();
  if (args.isMember("write_file") && args["write_file"].isBool()) opt.write_file = args["write_file"].asBool();
  if (args.isMember("kind")) {
    if (args["kind"].isString()) opt.kind = args["kind"].asString();
    else if (args["kind"].isNull()) opt.kind.clear();
  }

  if (args.isMember("model") && args["model"].isString()) opt.model = args["model"].asString();
  if (args.isMember("summary_model")) {
    if (args["summary_model"].isString()) opt.model = args["summary_model"].asString();
    else if (args["summary_model"].isNull()) opt.model.clear();
  }
  if (args.isMember("summary_max_chars") && args["summary_max_chars"].isInt64()) {
    const auto n = args["summary_max_chars"].asInt64();
    if (n >= 0) opt.summary_max_chars = (size_t)n;
  }

  if (args.isMember("include_structured") && args["include_structured"].isBool()) {
    opt.salience.include_structured = args["include_structured"].asBool();
  }
  if (args.isMember("include_daily") && args["include_daily"].isBool()) {
    opt.salience.include_daily = args["include_daily"].asBool();
  }
  if (args.isMember("daily_days") && args["daily_days"].isInt()) {
    opt.salience.daily_days = std::max(0, args["daily_days"].asInt());
  }
  if (args.isMember("max_items") && args["max_items"].isInt()) {
    opt.salience.max_items = std::max(0, args["max_items"].asInt());
  }
  if (args.isMember("max_structured_items") && args["max_structured_items"].isInt()) {
    opt.salience.max_structured_items = std::max(0, args["max_structured_items"].asInt());
  }
  if (args.isMember("max_daily_items") && args["max_daily_items"].isInt()) {
    opt.salience.max_daily_items = std::max(0, args["max_daily_items"].asInt());
  }
  if (args.isMember("half_life_days") && args["half_life_days"].isDouble()) {
    opt.salience.half_life_days = std::max(0.0, args["half_life_days"].asDouble());
  }
  if (args.isMember("importance_weight") && args["importance_weight"].isDouble()) {
    opt.salience.importance_weight = std::max(0.0, args["importance_weight"].asDouble());
  }

  opt.salience.daily_days = std::max(0, std::min(31, opt.salience.daily_days));
  opt.salience.max_items = std::max(0, std::min(200, opt.salience.max_items));
  opt.salience.max_structured_items = std::max(0, std::min(200, opt.salience.max_structured_items));
  opt.salience.max_daily_items = std::max(0, std::min(200, opt.salience.max_daily_items));

  MemoryRecapReport report;
  std::string err;
  if (!memory_generate_recap(cfg, ocfg, opt, &report, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err.empty() ? "memory recap failed" : err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o = memory_recap_report_to_json(report, /*include_prompt=*/report.dry_run);
  o["ok"] = true;
  if (!report.dry_run && report.write_file && !report.recap_path_rel.empty()) {
    MemoryCorrelationIndexReport idx_rep;
    std::string idx_err;
    MemoryCorrelationIndexOptions idx_opt = memory_correlation_index_default_options();
    if (memory_correlation_index_build(memory_root_from_cfg(cfg), idx_opt, &idx_rep, &idx_err)) {
      Json::Value ix(Json::objectValue);
      ix["ok"] = true;
      ix["generated_utc_ms"] = (Json::Int64)idx_rep.generated_utc_ms;
      if (!idx_rep.generated_utc.empty()) ix["generated_utc"] = idx_rep.generated_utc;
      ix["index_path"] = idx_rep.index_path;
      ix["token_count"] = (Json::Int64)idx_rep.token_count;
      ix["entry_count"] = (Json::Int64)idx_rep.entry_count;
      o["correlation_index"] = ix;
    } else if (!idx_err.empty()) {
      Json::Value ix(Json::objectValue);
      ix["ok"] = false;
      ix["error"] = idx_err;
      o["correlation_index"] = ix;
    }
  }
  resp->body = json_stringify(o);
}

}  // namespace agentd
