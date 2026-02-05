#include "memory_checkpoints.h"

#include "agent_sha256.h"
#include "json_util.h"

#include <algorithm>
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

}  // namespace

bool memory_list_structured_checkpoints(
  const std::filesystem::path& memory_root,
  int64_t since_utc_ms,
  int64_t until_utc_ms,
  const std::string& structured_path_filter,
  int limit,
  std::vector<MemoryCheckpointMeta>* out,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out) out->clear();
  if (!out) return false;

  if (since_utc_ms < 0) since_utc_ms = 0;
  if (until_utc_ms < since_utc_ms) std::swap(until_utc_ms, since_utc_ms);
  if (until_utc_ms <= 0) until_utc_ms = INT64_MAX;

  limit = std::max(1, std::min(200, limit));

  const std::filesystem::path ckdir = memory_root / "checkpoints";
  std::error_code ec;
  if (memory_root.empty() || !std::filesystem::exists(ckdir, ec) || !std::filesystem::is_directory(ckdir, ec)) {
    if (out_err) *out_err = "no checkpoints directory";
    return false;
  }

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
    if (!structured_path_filter.empty() && structured_path != structured_path_filter) continue;

    int64_t ts_ms = 0;
    if (!ts_utc.empty() && !parse_iso_utc_ms(ts_utc, &ts_ms)) continue;
    if (ts_ms < since_utc_ms || ts_ms > until_utc_ms) continue;

    char hex[65];
    agent_sha256_hex_of_bytes(text.data(), text.size(), hex);

    Row r;
    r.ts_ms = ts_ms;
    r.ts_utc = ts_utc;
    r.structured_path = structured_path;
    r.bytes = (int64_t)text.size();
    r.sha256 = std::string(hex);
    r.checkpoint_path_rel = de.path().lexically_relative(memory_root).generic_string();
    rows.push_back(std::move(r));
    if (rows.size() > 256) break;
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.ts_ms != b.ts_ms) return a.ts_ms > b.ts_ms;
    return a.checkpoint_path_rel > b.checkpoint_path_rel;
  });
  if ((int)rows.size() > limit) rows.resize((size_t)limit);

  out->reserve(rows.size());
  for (const auto& r : rows) {
    MemoryCheckpointMeta m;
    m.ts_utc_ms = r.ts_ms;
    m.ts_utc = r.ts_utc;
    m.checkpoint_path_rel = r.checkpoint_path_rel;
    m.structured_path = r.structured_path;
    m.sha256 = r.sha256;
    m.bytes = r.bytes;
    out->push_back(std::move(m));
  }
  return true;
}

bool memory_read_structured_checkpoint_items(
  const std::filesystem::path& memory_root,
  const std::string& checkpoint_path_rel,
  std::string* structured_path_out,
  Json::Value* items_out,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (structured_path_out) structured_path_out->clear();
  if (items_out) *items_out = Json::Value(Json::nullValue);
  if (!items_out || checkpoint_path_rel.empty() || memory_root.empty()) {
    if (out_err) *out_err = "invalid args";
    return false;
  }

  const std::filesystem::path abs = (memory_root / checkpoint_path_rel).lexically_normal();
  std::string text;
  if (!read_file_bounded(abs, /*max_bytes=*/10 * 1024 * 1024, &text)) {
    if (out_err) *out_err = "failed to read checkpoint";
    return false;
  }

  Json::Value ck;
  std::string perr;
  if (!json_parse_any(text, &ck, &perr) || !ck.isObject()) {
    if (out_err) *out_err = "failed to parse checkpoint JSON";
    return false;
  }

  const std::string structured_path =
    ck.isMember("path") && ck["path"].isString() ? ck["path"].asString() : "STRUCTURED.md";
  if (structured_path_out) *structured_path_out = structured_path;

  const Json::Value doc = ck.isMember("doc") ? ck["doc"] : Json::Value(Json::nullValue);
  const Json::Value items = doc.isObject() && doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
  if (!items.isObject()) {
    if (out_err) *out_err = "checkpoint missing doc.items";
    return false;
  }
  *items_out = items;
  return true;
}

}  // namespace agentd

