#include "workflow_memory_query.h"

#include "agent_sha256.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
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

Json::Value workflow_memory_query_to_json(
  const std::string& state_dir,
  const Json::Value& memory_query,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  if (!memory_query.isObject()) {
    if (out_err) *out_err = "memory_query must be an object";
    return err_out("memory_query must be an object");
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (memory_query.isMember("since_utc_ms") && (memory_query["since_utc_ms"].isInt64() || memory_query["since_utc_ms"].isUInt64() || memory_query["since_utc_ms"].isInt() || memory_query["since_utc_ms"].isUInt())) {
    since_ms = memory_query["since_utc_ms"].asInt64();
  }
  if (memory_query.isMember("until_utc_ms") && (memory_query["until_utc_ms"].isInt64() || memory_query["until_utc_ms"].isUInt64() || memory_query["until_utc_ms"].isInt() || memory_query["until_utc_ms"].isUInt())) {
    until_ms = memory_query["until_utc_ms"].asInt64();
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);
  if (since_ms < 0) since_ms = 0;

  std::string structured_path_filter =
    memory_query.isMember("structured_path") && memory_query["structured_path"].isString()
    ? trim_copy(memory_query["structured_path"].asString())
    : "";

  std::string key_prefix =
    memory_query.isMember("key_prefix") && memory_query["key_prefix"].isString()
    ? memory_query["key_prefix"].asString()
    : "";

  int limit = 50;
  if (memory_query.isMember("limit") && (memory_query["limit"].isInt() || memory_query["limit"].isUInt())) {
    limit = memory_query["limit"].asInt();
  }
  limit = std::max(1, std::min(1000, limit));

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
    if (!structured_path_filter.empty() && structured_path != structured_path_filter) continue;

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

  const CkRow& ckmeta = rows[0];
  const std::filesystem::path abs = (mem_root / ckmeta.checkpoint_path_rel).lexically_normal();
  std::string text;
  if (!read_file_bounded(abs, /*max_bytes=*/10 * 1024 * 1024, &text)) {
    if (out_err) *out_err = "failed to read checkpoint";
    return err_out("failed to read checkpoint");
  }
  Json::Value ck;
  std::string err;
  if (!json_parse_any(text, &ck, &err) || !ck.isObject()) {
    if (out_err) *out_err = "failed to parse checkpoint JSON";
    return err_out("failed to parse checkpoint JSON");
  }
  const Json::Value doc = ck.isMember("doc") ? ck["doc"] : Json::Value(Json::nullValue);
  const Json::Value items = doc.isObject() && doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
  if (!items.isObject()) {
    if (out_err) *out_err = "checkpoint missing doc.items";
    return err_out("checkpoint missing doc.items");
  }

  std::vector<std::string> keys = items.getMemberNames();
  std::sort(keys.begin(), keys.end());

  Json::Value entries(Json::arrayValue);
  Json::Value entries_by_key(Json::objectValue);
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
    entries_by_key[key] = rec;
    returned++;
  }

  Json::Value ckinfo(Json::objectValue);
  ckinfo["checkpoint_path"] = ckmeta.checkpoint_path_rel;
  ckinfo["structured_path"] = ckmeta.structured_path;
  ckinfo["sha256"] = ckmeta.sha256;
  ckinfo["ts_utc"] = ckmeta.ts_utc;
  ckinfo["ts_utc_ms"] = (Json::Int64)ckmeta.ts_ms;
  ckinfo["bytes"] = (Json::Int64)ckmeta.bytes;

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["assistant_text"] = "memory_query: ok";
  out["memory_root"] = mem_root.generic_string();
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  if (!structured_path_filter.empty()) out["structured_path_filter"] = structured_path_filter;
  if (!key_prefix.empty()) out["key_prefix"] = key_prefix;
  out["limit"] = limit;
  out["returned"] = returned;
  out["checkpoint"] = ckinfo;
  out["entries"] = entries;
  out["entries_by_key"] = entries_by_key;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  return out;
}

}  // namespace agentd

