#include "memory_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "memory_consolidator.h"

#include "agent_sha256.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {

namespace {

static int64_t timegm_utc(std::tm* tm) {
  if (!tm) return 0;
#if defined(_WIN32)
  return (int64_t)_mkgmtime(tm);
#else
  return (int64_t)timegm(tm);
#endif
}

static bool parse_iso_utc_ms(const std::string& s, int64_t* out_unix_ms) {
  if (out_unix_ms) *out_unix_ms = 0;
  if (!out_unix_ms) return false;
  // Expected: YYYY-MM-DDTHH:MM:SSZ
  if (s.size() != 20) return false;
  auto dig2 = [&](size_t off) -> int {
    if (off + 1 >= s.size()) return -1;
    const char a = s[off];
    const char b = s[off + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
  };
  auto dig4 = [&](size_t off) -> int {
    if (off + 3 >= s.size()) return -1;
    int v = 0;
    for (size_t i = 0; i < 4; i++) {
      const char c = s[off + i];
      if (c < '0' || c > '9') return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };

  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
  const int year = dig4(0);
  const int mon = dig2(5);
  const int day = dig2(8);
  const int hour = dig2(11);
  const int min = dig2(14);
  const int sec = dig2(17);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
    return false;
  }

  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  const int64_t t = timegm_utc(&tm);
  if (t <= 0) return false;
  *out_unix_ms = t * 1000;
  return true;
}

static bool read_file_bounded(const std::filesystem::path& p, size_t max_bytes, std::string* out) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  const uintmax_t sz = std::filesystem::file_size(p, ec);
  if (ec) return false;
  if (sz > max_bytes) return false;
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

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

  struct Row {
    int64_t ts_ms = 0;
    std::string ts_utc;
    std::string checkpoint_path_rel;
    std::string structured_path;
    std::string sha256;
    int64_t bytes = 0;
  };
  std::vector<Row> rows;
  rows.reserve((size_t)limit);

  for (auto it = std::filesystem::directory_iterator(ckdir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
    const auto& de = *it;
    if (!de.is_regular_file(ec)) continue;
    const std::string fn = de.path().filename().string();
    if (fn.rfind("structured_", 0) != 0) continue;
    if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;

    std::string text;
    if (!read_file_bounded(de.path(), /*max_bytes=*/10 * 1024 * 1024, &text)) continue;
    if (text.empty()) continue;

    Json::Value ck;
    std::string perr;
    if (!json_parse_any(text, &ck, &perr) || !ck.isObject()) continue;
    const std::string ts_utc = ck.isMember("ts_utc") && ck["ts_utc"].isString() ? ck["ts_utc"].asString() : "";
    const std::string structured_path =
      ck.isMember("path") && ck["path"].isString() ? ck["path"].asString() : "STRUCTURED.md";

    int64_t ts_ms = 0;
    if (!ts_utc.empty() && !parse_iso_utc_ms(ts_utc, &ts_ms)) continue;
    if (ts_ms < since_ms || ts_ms > until_ms) continue;
    if (!structured_path_filter.empty() && structured_path != structured_path_filter) continue;

    char hex[65];
    agent_sha256_hex_of_bytes(text.data(), text.size(), hex);

    Row r;
    r.ts_ms = ts_ms;
    r.ts_utc = ts_utc;
    r.structured_path = structured_path;
    r.bytes = (int64_t)text.size();
    r.sha256 = std::string(hex);
    r.checkpoint_path_rel = de.path().lexically_relative(mem_root).generic_string();
    rows.push_back(std::move(r));
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.ts_ms != b.ts_ms) return a.ts_ms > b.ts_ms;
    return a.checkpoint_path_rel > b.checkpoint_path_rel;
  });
  if ((int)rows.size() > limit) rows.resize((size_t)limit);

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
    row["ts_utc_ms"] = (Json::Int64)r.ts_ms;
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

  // Reuse the checkpoints listing logic but keep only the newest by default.
  HttpRequest req2 = req;
  if (!timeline) {
    if (req2.query.empty()) {
      req2.query = "limit=1";
    } else {
      req2.query = "limit=1&" + req2.query;
    }
  }
  HttpResponse tmp;
  handle_memory_checkpoints_endpoint(cfg, cors_cfg, req2, &tmp);
  if (tmp.status != 200) {
    *resp = tmp;
    return;
  }

  Json::Value list;
  std::string perr;
  if (!json_parse_any(tmp.body, &list, &perr) || !list.isObject()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"failed to parse internal checkpoint listing\"}";
    return;
  }
  const Json::Value cps = list.isMember("checkpoints") ? list["checkpoints"] : Json::Value(Json::nullValue);
  if (!cps.isArray() || cps.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints in window\"}";
    return;
  }

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  const std::string needle = std::string("trace:") + tid;

  auto extract_entries = [&](const Json::Value& ckmeta, Json::Value* out_entries, Json::Value* out_ckinfo) -> bool {
    if (!out_entries || !out_ckinfo) return false;
    *out_entries = Json::Value(Json::arrayValue);
    *out_ckinfo = Json::Value(Json::objectValue);
    if (!ckmeta.isObject()) return false;
    const std::string ckrel = ckmeta.isMember("checkpoint_path") && ckmeta["checkpoint_path"].isString() ? ckmeta["checkpoint_path"].asString() : "";
    if (ckrel.empty()) return false;
    const std::filesystem::path abs = (mem_root / ckrel).lexically_normal();

    std::string text;
    if (!read_file_bounded(abs, /*max_bytes=*/10 * 1024 * 1024, &text)) return false;
    Json::Value ck;
    std::string err;
    if (!json_parse_any(text, &ck, &err) || !ck.isObject()) return false;

    const std::string structured_path =
      ck.isMember("path") && ck["path"].isString() ? ck["path"].asString() : "STRUCTURED.md";

    // Copy checkpoint info.
    (*out_ckinfo)["checkpoint_path"] = ckrel;
    if (ckmeta.isMember("sha256")) (*out_ckinfo)["sha256"] = ckmeta["sha256"];
    if (ckmeta.isMember("ts_utc")) (*out_ckinfo)["ts_utc"] = ckmeta["ts_utc"];
    if (ckmeta.isMember("ts_utc_ms")) (*out_ckinfo)["ts_utc_ms"] = ckmeta["ts_utc_ms"];
    (*out_ckinfo)["structured_path"] = structured_path;

    if (!structured_path_filter.empty() && structured_path != structured_path_filter) return true;

    const Json::Value doc = ck.isMember("doc") ? ck["doc"] : Json::Value(Json::nullValue);
    const Json::Value items = doc.isObject() && doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
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
        if (s.find(needle) != std::string::npos) {
          hit = true;
          break;
        }
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
    const Json::Value newest = cps[0];
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
    for (Json::ArrayIndex i = 0; i < cps.size(); i++) {
      Json::Value entries, ckinfo;
      if (!extract_entries(cps[i], &entries, &ckinfo)) continue;
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

  // Reuse the checkpoints listing logic but keep only the newest checkpoint by default.
  HttpRequest req2 = req;
  if (req2.query.empty()) {
    req2.query = "limit=1";
  } else {
    req2.query = "limit=1&" + req2.query;
  }
  HttpResponse tmp;
  handle_memory_checkpoints_endpoint(cfg, cors_cfg, req2, &tmp);
  if (tmp.status != 200) {
    *resp = tmp;
    return;
  }

  Json::Value list;
  std::string perr;
  if (!json_parse_any(tmp.body, &list, &perr) || !list.isObject()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"failed to parse internal checkpoint listing\"}";
    return;
  }
  const Json::Value cps = list.isMember("checkpoints") ? list["checkpoints"] : Json::Value(Json::nullValue);
  if (!cps.isArray() || cps.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints in window\"}";
    return;
  }

  const std::filesystem::path mem_root = memory_root_from_cfg(cfg);
  if (mem_root.empty()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"missing memory root\"}";
    return;
  }
  const Json::Value newest = cps[0];
  const std::string ckrel =
    newest.isObject() && newest.isMember("checkpoint_path") && newest["checkpoint_path"].isString()
      ? newest["checkpoint_path"].asString()
      : "";
  if (ckrel.empty()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"invalid checkpoint metadata\"}";
    return;
  }
  const std::filesystem::path abs = (mem_root / ckrel).lexically_normal();

  std::string text;
  if (!read_file_bounded(abs, /*max_bytes=*/10 * 1024 * 1024, &text)) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"failed to read checkpoint\"}";
    return;
  }
  Json::Value ck;
  std::string err;
  if (!json_parse_any(text, &ck, &err) || !ck.isObject()) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"failed to parse checkpoint JSON\"}";
    return;
  }

  const std::string structured_path =
    ck.isMember("path") && ck["path"].isString() ? ck["path"].asString() : "STRUCTURED.md";
  if (!structured_path_filter.empty() && structured_path != structured_path_filter) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"no checkpoints matching structured_path in window\"}";
    return;
  }

  const Json::Value doc = ck.isMember("doc") ? ck["doc"] : Json::Value(Json::nullValue);
  const Json::Value items = doc.isObject() && doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
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
  out["checkpoint"] = newest;
  if (out["checkpoint"].isObject()) out["checkpoint"]["structured_path"] = structured_path;
  out["entries"] = entries;
  out["returned"] = returned;
  resp->body = json_stringify(out);
}

}  // namespace agentd
