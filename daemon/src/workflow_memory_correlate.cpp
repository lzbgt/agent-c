#include "workflow_memory_correlate.h"

#include "agent_sha256.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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

static std::filesystem::path memory_root_from_state_dir(const std::string& state_dir) {
  if (state_dir.empty()) return {};
  return (std::filesystem::path(state_dir) / "memory").lexically_normal();
}

static Json::Value err_out(const std::string& err) {
  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["assistant_text"] = "";
  out["error"] = err;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  return out;
}

}  // namespace

Json::Value workflow_memory_correlate_to_json(
  const std::string& state_dir,
  const std::string& workflow_trace_id,
  const Json::Value& memory_correlate,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  if (!memory_correlate.isObject()) {
    if (out_err) *out_err = "memory_correlate must be an object";
    return err_out("memory_correlate must be an object");
  }

  std::string trace_id =
    memory_correlate.isMember("trace_id") && memory_correlate["trace_id"].isString()
    ? trim_copy(memory_correlate["trace_id"].asString())
    : "";
  if (trace_id.empty()) trace_id = workflow_trace_id;
  if (trace_id.empty()) {
    if (out_err) *out_err = "memory_correlate.trace_id is required (or workflow trace_id must be set)";
    return err_out("memory_correlate.trace_id is required (or workflow trace_id must be set)");
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (memory_correlate.isMember("since_utc_ms") && (memory_correlate["since_utc_ms"].isInt64() || memory_correlate["since_utc_ms"].isUInt64() || memory_correlate["since_utc_ms"].isInt() || memory_correlate["since_utc_ms"].isUInt())) {
    since_ms = memory_correlate["since_utc_ms"].asInt64();
  }
  if (memory_correlate.isMember("until_utc_ms") && (memory_correlate["until_utc_ms"].isInt64() || memory_correlate["until_utc_ms"].isUInt64() || memory_correlate["until_utc_ms"].isInt() || memory_correlate["until_utc_ms"].isUInt())) {
    until_ms = memory_correlate["until_utc_ms"].asInt64();
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);
  if (since_ms < 0) since_ms = 0;

  int max_entries = 50;
  if (memory_correlate.isMember("max_entries") && (memory_correlate["max_entries"].isInt() || memory_correlate["max_entries"].isUInt())) {
    max_entries = memory_correlate["max_entries"].asInt();
  }
  max_entries = std::max(1, std::min(500, max_entries));

  const bool timeline =
    memory_correlate.isMember("timeline") && memory_correlate["timeline"].isBool() ? memory_correlate["timeline"].asBool() : false;

  const std::filesystem::path mem_root = memory_root_from_state_dir(state_dir);
  const std::filesystem::path ckdir = mem_root / "checkpoints";
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(ckdir, ec) || !std::filesystem::is_directory(ckdir, ec)) {
    if (out_err) *out_err = "no checkpoints directory";
    return err_out("no checkpoints directory");
  }

  struct CkRow {
    int64_t ts_ms = 0;
    std::string ts_utc;
    std::string checkpoint_path_rel;
    std::string structured_path;
    std::string sha256;
    int64_t bytes = 0;
  };
  std::vector<CkRow> rows;
  rows.reserve(64);

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

    char hex[65];
    agent_sha256_hex_of_bytes(text.data(), text.size(), hex);

    CkRow r;
    r.ts_ms = ts_ms;
    r.ts_utc = ts_utc;
    r.structured_path = structured_path;
    r.bytes = (int64_t)text.size();
    r.sha256 = std::string(hex);
    r.checkpoint_path_rel = de.path().lexically_relative(mem_root).generic_string();
    rows.push_back(std::move(r));
    if (rows.size() > 256) break;
  }

  std::sort(rows.begin(), rows.end(), [](const CkRow& a, const CkRow& b) {
    if (a.ts_ms != b.ts_ms) return a.ts_ms > b.ts_ms;
    return a.checkpoint_path_rel > b.checkpoint_path_rel;
  });
  if (rows.empty()) {
    if (out_err) *out_err = "no checkpoints in window";
    return err_out("no checkpoints in window");
  }

  const std::string needle = std::string("trace:") + trace_id;

  auto extract_entries = [&](const CkRow& ckmeta, Json::Value* out_entries, Json::Value* out_ckinfo) -> bool {
    if (!out_entries || !out_ckinfo) return false;
    *out_entries = Json::Value(Json::arrayValue);
    *out_ckinfo = Json::Value(Json::objectValue);

    if (ckmeta.checkpoint_path_rel.empty()) return false;
    const std::filesystem::path abs = (mem_root / ckmeta.checkpoint_path_rel).lexically_normal();
    std::string text;
    if (!read_file_bounded(abs, /*max_bytes=*/10 * 1024 * 1024, &text)) return false;
    Json::Value ck;
    std::string err;
    if (!json_parse_any(text, &ck, &err) || !ck.isObject()) return false;
    const Json::Value doc = ck.isMember("doc") ? ck["doc"] : Json::Value(Json::nullValue);
    const Json::Value items = doc.isObject() && doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
    if (!items.isObject()) return false;

    (*out_ckinfo)["checkpoint_path"] = ckmeta.checkpoint_path_rel;
    (*out_ckinfo)["structured_path"] = ckmeta.structured_path;
    (*out_ckinfo)["sha256"] = ckmeta.sha256;
    (*out_ckinfo)["ts_utc"] = ckmeta.ts_utc;
    (*out_ckinfo)["ts_utc_ms"] = (Json::Int64)ckmeta.ts_ms;
    (*out_ckinfo)["bytes"] = (Json::Int64)ckmeta.bytes;

    int added = 0;
    for (const auto& key : items.getMemberNames()) {
      if (added >= max_entries) break;
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
  out["trace_id"] = trace_id;
  out["needle"] = needle;
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;

  if (!timeline) {
    Json::Value entries, ckinfo;
    if (!extract_entries(rows[0], &entries, &ckinfo)) {
      if (out_err) *out_err = "failed to extract entries from newest checkpoint";
      return err_out("failed to extract entries from newest checkpoint");
    }
    out["checkpoint"] = ckinfo;
    out["entries"] = entries;
    out["assistant_text"] = "memory_correlate entries=" + std::to_string((unsigned)entries.size());
    return out;
  }

  Json::Value timeline_arr(Json::arrayValue);
  for (size_t i = 0; i < rows.size(); i++) {
    Json::Value entries, ckinfo;
    if (!extract_entries(rows[i], &entries, &ckinfo)) continue;
    Json::Value row(Json::objectValue);
    row["checkpoint"] = ckinfo;
    row["entries"] = entries;
    timeline_arr.append(row);
    if ((int)timeline_arr.size() >= 50) break;
  }
  out["timeline"] = timeline_arr;
  out["assistant_text"] = "memory_correlate timeline=" + std::to_string((unsigned)timeline_arr.size());
  return out;
}

}  // namespace agentd

