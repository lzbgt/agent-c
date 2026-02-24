#include "memory_correlation_index.h"

#include "json_util.h"
#include "memory_checkpoints.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string iso_utc_now() {
  const auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

std::string trim_ascii(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string truncate_ascii(std::string s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  if (max_chars < 3) return s.substr(0, max_chars);
  s.resize(max_chars - 3);
  s += "...";
  return s;
}

std::string local_date_ymd_days_ago(int days_ago) {
  const auto now = std::chrono::system_clock::now() - std::chrono::hours(24LL * (long long)days_ago);
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

bool read_file_bounded(const std::filesystem::path& p, size_t max_bytes, std::string* out) {
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

bool parse_iso_utc_ms(const std::string& s, int64_t* out_unix_ms) {
  if (!out_unix_ms) return false;
  *out_unix_ms = 0;
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
#if defined(_WIN32)
  const int64_t t = (int64_t)_mkgmtime(&tm);
#else
  const int64_t t = (int64_t)timegm(&tm);
#endif
  if (t <= 0) return false;
  *out_unix_ms = t * 1000;
  return true;
}

std::string normalize_token(std::string s) {
  if (s.empty()) return s;
  while (!s.empty() && (s.back() == ',' || s.back() == ';' || s.back() == '.' || s.back() == ')' || s.back() == ']')) {
    s.pop_back();
  }
  while (!s.empty() && (s.front() == '(' || s.front() == '[' || s.front() == '"' || s.front() == '\'')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == '"' || s.back() == '\'')) {
    s.pop_back();
  }
  return s;
}

bool token_prefix_allowed(const std::string& token) {
  const size_t pos = token.find(':');
  if (pos == std::string::npos || pos == 0) return false;
  const std::string prefix = token.substr(0, pos);
  return prefix == "trace" || prefix == "workflow" || prefix == "session" || prefix == "task" || prefix == "run" || prefix == "job";
}

std::vector<std::string> extract_tokens_from_source(const std::string& src) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add_token = [&](const std::string& t) {
    if (t.empty()) return;
    if (!token_prefix_allowed(t)) return;
    if (t.size() > 200) return;
    if (seen.insert(t).second) out.push_back(t);
  };
  std::istringstream iss(src);
  std::string tok;
  while (iss >> tok) {
    tok = normalize_token(tok);
    if (tok.empty()) continue;
    add_token(tok);
    if (tok.rfind("trace:", 0) == 0) {
      const std::string rest = tok.substr(6);
      const size_t extra = rest.find(':');
      if (extra != std::string::npos && extra > 0) {
        add_token(std::string("trace:") + rest.substr(0, extra));
      }
    }
  }
  return out;
}

void extract_tokens_from_sources(const Json::Value& sources, std::vector<std::string>* out) {
  if (!out) return;
  if (!sources.isArray()) return;
  std::unordered_set<std::string> seen(out->begin(), out->end());
  for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
    if (!sources[i].isString()) continue;
    const std::string src = sources[i].asString();
    for (const auto& tok : extract_tokens_from_source(src)) {
      if (seen.insert(tok).second) out->push_back(tok);
    }
  }
}

struct RecapRef {
  std::string path_rel;
  std::string kind;
  std::string ts_utc;
  int64_t ts_ms = 0;
  std::string summary_excerpt;
  std::vector<std::string> evidence_sources;
};

void load_recap_sources(
  const std::filesystem::path& mem_root,
  int max_recaps,
  size_t excerpt_chars,
  std::unordered_map<std::string, std::vector<RecapRef>>* out_map
) {
  if (!out_map) return;
  out_map->clear();
  const std::filesystem::path recap_dir = mem_root / "recaps";
  std::error_code ec;
  if (!std::filesystem::exists(recap_dir, ec) || !std::filesystem::is_directory(recap_dir, ec)) return;

  int loaded = 0;
  for (auto it = std::filesystem::directory_iterator(recap_dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
    if (loaded >= max_recaps) break;
    const auto& de = *it;
    if (!de.is_regular_file(ec)) continue;
    const std::string fn = de.path().filename().string();
    if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;

    std::string text;
    if (!read_file_bounded(de.path(), /*max_bytes=*/4 * 1024 * 1024, &text)) continue;
    if (text.empty()) continue;

    Json::Value doc;
    std::string perr;
    if (!json_parse_any(text, &doc, &perr) || !doc.isObject()) continue;

    RecapRef ref;
    ref.path_rel = de.path().lexically_relative(mem_root).generic_string();
    ref.kind = doc.isMember("kind") && doc["kind"].isString() ? doc["kind"].asString() : "";
    ref.ts_utc = doc.isMember("ts_utc") && doc["ts_utc"].isString() ? doc["ts_utc"].asString() : "";
    if (!ref.ts_utc.empty()) {
      (void)parse_iso_utc_ms(ref.ts_utc, &ref.ts_ms);
    }
    const std::string summary_text = doc.isMember("summary_text") && doc["summary_text"].isString() ? doc["summary_text"].asString() : "";
    ref.summary_excerpt = truncate_ascii(trim_ascii(summary_text), excerpt_chars);

    const Json::Value sources = doc.isMember("evidence_sources") ? doc["evidence_sources"] : Json::Value(Json::nullValue);
    if (sources.isArray()) {
      for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
        if (!sources[i].isString()) continue;
        const std::string s = trim_ascii(sources[i].asString());
        if (s.empty()) continue;
        ref.evidence_sources.push_back(s);
      }
    }

    for (const auto& src : ref.evidence_sources) {
      (*out_map)[src].push_back(ref);
    }

    loaded++;
  }
}

struct DailyObs {
  std::string path_rel;
  int line = 1;
  std::string text;
  std::string trace_id;
  std::string source;
  std::string ts_utc;
  int importance = -1;
};

bool line_starts_with_obs(const std::string& trimmed) {
  if (trimmed.rfind("- @obs", 0) == 0) return true;
  if (trimmed.rfind("* @obs", 0) == 0) return true;
  if (trimmed.rfind("@obs", 0) == 0) return true;
  return false;
}

void parse_daily_obs_file(const std::filesystem::path& abs_path, const std::string& rel_path, int max_entries, std::vector<DailyObs>* out) {
  if (!out) return;
  std::ifstream in(abs_path);
  if (!in.is_open()) return;

  DailyObs cur;
  bool in_obs = false;
  int line_no = 0;

  auto flush = [&]() {
    if (in_obs && !cur.trace_id.empty()) {
      out->push_back(cur);
    }
    in_obs = false;
    cur = DailyObs{};
  };

  std::string line;
  while (std::getline(in, line)) {
    line_no++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string trimmed = trim_ascii(line);
    if (trimmed.empty()) continue;

    if (line_starts_with_obs(trimmed)) {
      flush();
      in_obs = true;
      cur.path_rel = rel_path;
      cur.line = line_no;
      std::string text = trimmed;
      const size_t pos = text.find("@obs");
      if (pos != std::string::npos) {
        text = trim_ascii(text.substr(pos + 4));
      }
      cur.text = text;
      if ((int)out->size() >= max_entries) break;
      continue;
    }

    if (!in_obs) continue;

    if (line_starts_with_obs(trimmed)) {
      flush();
      in_obs = true;
      cur.path_rel = rel_path;
      cur.line = line_no;
      std::string text = trimmed;
      const size_t pos = text.find("@obs");
      if (pos != std::string::npos) {
        text = trim_ascii(text.substr(pos + 4));
      }
      cur.text = text;
      if ((int)out->size() >= max_entries) break;
      continue;
    }

    if (trimmed.rfind("- ", 0) == 0) {
      std::string kv = trim_ascii(trimmed.substr(2));
      const size_t sep = kv.find(':');
      if (sep == std::string::npos) continue;
      const std::string key = trim_ascii(kv.substr(0, sep));
      const std::string value = trim_ascii(kv.substr(sep + 1));
      if (key == "trace_id") cur.trace_id = value;
      else if (key == "source") cur.source = value;
      else if (key == "ts_utc") cur.ts_utc = value;
      else if (key == "importance") {
        try {
          cur.importance = std::stoi(value);
        } catch (...) {
          cur.importance = -1;
        }
      }
    }
  }
  flush();
}

struct IndexEntry {
  std::string token;
  std::string kind;
  std::string ref;
  int64_t ts_ms = 0;
  Json::Value payload{Json::objectValue};
};

std::filesystem::path index_sqlite_path(const std::filesystem::path& mem_root) {
  return mem_root / ".memory_correlation.sqlite3";
}

std::filesystem::path index_json_path(const std::filesystem::path& mem_root) {
  return mem_root / ".memory_correlation.json";
}

#if defined(AGENT_HAVE_SQLITE3)

bool sqlite_exec(sqlite3* db, const char* sql, std::string* out_err) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    if (out_err) {
      *out_err = err ? std::string(err) : "sqlite exec failed";
    }
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool sqlite_write_index(
  const std::filesystem::path& path,
  const MemoryCorrelationIndexReport& rep,
  const std::vector<IndexEntry>& entries,
  std::string* out_err
) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
    if (out_err) *out_err = "failed to open correlation index sqlite";
    if (db) sqlite3_close(db);
    return false;
  }

  if (!sqlite_exec(db, "PRAGMA journal_mode=WAL;", out_err)) {
    sqlite3_close(db);
    return false;
  }
  if (!sqlite_exec(db, "PRAGMA synchronous=NORMAL;", out_err)) {
    sqlite3_close(db);
    return false;
  }

  if (!sqlite_exec(db, "BEGIN;", out_err)) {
    sqlite3_close(db);
    return false;
  }

  if (!sqlite_exec(db, "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);", out_err)) {
    sqlite3_close(db);
    return false;
  }
  if (!sqlite_exec(db, "CREATE TABLE IF NOT EXISTS entries (token TEXT, kind TEXT, ref TEXT, ts_utc_ms INTEGER, payload_json TEXT);", out_err)) {
    sqlite3_close(db);
    return false;
  }
  if (!sqlite_exec(db, "DELETE FROM meta;", out_err)) {
    sqlite3_close(db);
    return false;
  }
  if (!sqlite_exec(db, "DELETE FROM entries;", out_err)) {
    sqlite3_close(db);
    return false;
  }
  if (!sqlite_exec(db, "CREATE INDEX IF NOT EXISTS idx_entries_token ON entries(token);", out_err)) {
    sqlite3_close(db);
    return false;
  }

  sqlite3_stmt* stmt_meta = nullptr;
  const char* meta_sql = "INSERT INTO meta (key, value) VALUES (?, ?);";
  if (sqlite3_prepare_v2(db, meta_sql, -1, &stmt_meta, nullptr) != SQLITE_OK) {
    if (out_err) *out_err = "failed to prepare meta insert";
    sqlite3_close(db);
    return false;
  }

  auto insert_meta = [&](const std::string& key, const std::string& value) -> bool {
    sqlite3_reset(stmt_meta);
    sqlite3_bind_text(stmt_meta, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_meta, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt_meta);
    return rc == SQLITE_DONE;
  };

  if (!insert_meta("schema", "agentd_memory_correlation_index_v1")) {
    if (out_err) *out_err = "failed to insert schema meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!insert_meta("generated_utc_ms", std::to_string(rep.generated_utc_ms))) {
    if (out_err) *out_err = "failed to insert generated_utc_ms meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!rep.generated_utc.empty()) {
    if (!insert_meta("generated_utc", rep.generated_utc)) {
      if (out_err) *out_err = "failed to insert generated_utc meta";
      sqlite3_finalize(stmt_meta);
      sqlite3_close(db);
      return false;
    }
  }
  if (!insert_meta("token_count", std::to_string(rep.token_count))) {
    if (out_err) *out_err = "failed to insert token_count meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!insert_meta("entry_count", std::to_string(rep.entry_count))) {
    if (out_err) *out_err = "failed to insert entry_count meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!insert_meta("structured_entries", std::to_string(rep.structured_entries))) {
    if (out_err) *out_err = "failed to insert structured_entries meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!insert_meta("daily_entries", std::to_string(rep.daily_entries))) {
    if (out_err) *out_err = "failed to insert daily_entries meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }
  if (!insert_meta("recap_entries", std::to_string(rep.recap_entries))) {
    if (out_err) *out_err = "failed to insert recap_entries meta";
    sqlite3_finalize(stmt_meta);
    sqlite3_close(db);
    return false;
  }

  sqlite3_finalize(stmt_meta);

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO entries (token, kind, ref, ts_utc_ms, payload_json) VALUES (?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (out_err) *out_err = "failed to prepare entries insert";
    sqlite3_close(db);
    return false;
  }

  for (const auto& entry : entries) {
    Json::Value payload = entry.payload;
    payload["kind"] = entry.kind;
    payload["ref"] = entry.ref;
    payload["token"] = entry.token;
    payload["ts_utc_ms"] = (Json::Int64)entry.ts_ms;
    const std::string payload_json = json_stringify(payload);

    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, entry.token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, entry.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, entry.ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)entry.ts_ms);
    sqlite3_bind_text(stmt, 5, payload_json.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      if (out_err) *out_err = "failed to insert correlation entry";
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }
  }

  sqlite3_finalize(stmt);
  if (!sqlite_exec(db, "COMMIT;", out_err)) {
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

bool sqlite_query_index(
  const std::filesystem::path& path,
  const std::string& token,
  int max_entries,
  Json::Value* out_entries,
  Json::Value* out_meta,
  std::string* out_err
) {
  if (out_entries) *out_entries = Json::Value(Json::arrayValue);
  if (out_meta) *out_meta = Json::Value(Json::objectValue);

  sqlite3* db = nullptr;
  if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
    if (out_err) *out_err = "failed to open correlation index sqlite";
    if (db) sqlite3_close(db);
    return false;
  }

  sqlite3_stmt* stmt_meta = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT key, value FROM meta;", -1, &stmt_meta, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt_meta) == SQLITE_ROW) {
      const char* k = (const char*)sqlite3_column_text(stmt_meta, 0);
      const char* v = (const char*)sqlite3_column_text(stmt_meta, 1);
      if (!k || !v) continue;
      if (out_meta) {
        std::string key(k);
        std::string val(v);
        if (key.find("_ms") != std::string::npos || key.find("count") != std::string::npos || key.find("entries") != std::string::npos) {
          try {
            (*out_meta)[key] = (Json::Int64)std::stoll(val);
          } catch (...) {
            (*out_meta)[key] = val;
          }
        } else {
          (*out_meta)[key] = val;
        }
      }
    }
  }
  if (stmt_meta) sqlite3_finalize(stmt_meta);

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT kind, ref, ts_utc_ms, payload_json FROM entries WHERE token = ? ORDER BY ts_utc_ms DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (out_err) *out_err = "failed to prepare correlation query";
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, std::max(1, max_entries));

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* payload = (const char*)sqlite3_column_text(stmt, 3);
    if (!payload) continue;
    Json::Value entry(Json::objectValue);
    std::string perr;
    if (!json_parse_any(std::string(payload), &entry, &perr) || !entry.isObject()) {
      continue;
    }
    if (out_entries) out_entries->append(entry);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

#endif  // AGENT_HAVE_SQLITE3

bool json_write_index(
  const std::filesystem::path& path,
  const MemoryCorrelationIndexReport& rep,
  const std::vector<IndexEntry>& entries,
  std::string* out_err
) {
  Json::Value doc(Json::objectValue);
  doc["schema"] = "agentd_memory_correlation_index_v1";
  doc["generated_utc_ms"] = (Json::Int64)rep.generated_utc_ms;
  if (!rep.generated_utc.empty()) doc["generated_utc"] = rep.generated_utc;
  doc["token_count"] = (Json::Int64)rep.token_count;
  doc["entry_count"] = (Json::Int64)rep.entry_count;
  doc["structured_entries"] = (Json::Int64)rep.structured_entries;
  doc["daily_entries"] = (Json::Int64)rep.daily_entries;
  doc["recap_entries"] = (Json::Int64)rep.recap_entries;

  Json::Value arr(Json::arrayValue);
  for (const auto& entry : entries) {
    Json::Value payload = entry.payload;
    payload["kind"] = entry.kind;
    payload["ref"] = entry.ref;
    payload["token"] = entry.token;
    payload["ts_utc_ms"] = (Json::Int64)entry.ts_ms;
    arr.append(payload);
  }
  doc["entries"] = arr;

  const std::string text = json_stringify(doc);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (out_err) *out_err = "failed to write correlation index json";
    return false;
  }
  out.write(text.data(), (std::streamsize)text.size());
  if (!out.good()) {
    if (out_err) *out_err = "failed to write correlation index json";
    return false;
  }
  return true;
}

bool json_query_index(
  const std::filesystem::path& path,
  const std::string& token,
  int max_entries,
  Json::Value* out_entries,
  Json::Value* out_meta,
  std::string* out_err
) {
  if (out_entries) *out_entries = Json::Value(Json::arrayValue);
  if (out_meta) *out_meta = Json::Value(Json::objectValue);

  std::string text;
  if (!read_file_bounded(path, /*max_bytes=*/10 * 1024 * 1024, &text)) {
    if (out_err) *out_err = "failed to read correlation index";
    return false;
  }
  if (text.empty()) {
    if (out_err) *out_err = "correlation index empty";
    return false;
  }

  Json::Value doc;
  std::string perr;
  if (!json_parse_any(text, &doc, &perr) || !doc.isObject()) {
    if (out_err) *out_err = "failed to parse correlation index json";
    return false;
  }

  if (out_meta) {
    (*out_meta)["schema"] = doc.get("schema", "");
    (*out_meta)["generated_utc_ms"] = doc.get("generated_utc_ms", 0);
    if (doc.isMember("generated_utc")) (*out_meta)["generated_utc"] = doc["generated_utc"];
    if (doc.isMember("token_count")) (*out_meta)["token_count"] = doc["token_count"];
    if (doc.isMember("entry_count")) (*out_meta)["entry_count"] = doc["entry_count"];
    if (doc.isMember("structured_entries")) (*out_meta)["structured_entries"] = doc["structured_entries"];
    if (doc.isMember("daily_entries")) (*out_meta)["daily_entries"] = doc["daily_entries"];
    if (doc.isMember("recap_entries")) (*out_meta)["recap_entries"] = doc["recap_entries"];
  }

  const Json::Value entries = doc.isMember("entries") ? doc["entries"] : Json::Value(Json::nullValue);
  if (!entries.isArray()) return true;

  int added = 0;
  for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
    if (added >= max_entries) break;
    const Json::Value e = entries[i];
    if (!e.isObject()) continue;
    if (!e.isMember("token") || !e["token"].isString()) continue;
    if (e["token"].asString() != token) continue;
    if (out_entries) out_entries->append(e);
    added++;
  }
  return true;
}

}  // namespace

MemoryCorrelationIndexOptions memory_correlation_index_default_options() {
  MemoryCorrelationIndexOptions opt;
  return opt;
}

bool memory_correlation_index_build(
  const std::filesystem::path& memory_root,
  const MemoryCorrelationIndexOptions& opt,
  MemoryCorrelationIndexReport* out_report,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_report) *out_report = MemoryCorrelationIndexReport{};

  if (memory_root.empty()) {
    if (out_err) *out_err = "memory root missing";
    return false;
  }

  MemoryCorrelationIndexReport rep;
  rep.generated_utc_ms = now_utc_ms();
  rep.generated_utc = iso_utc_now();

  const std::filesystem::path mem_root = memory_root.lexically_normal();
  std::unordered_map<std::string, std::vector<RecapRef>> recaps_by_source;
  if (opt.include_recaps) {
    load_recap_sources(mem_root, std::max(1, opt.max_recaps), opt.value_excerpt_chars, &recaps_by_source);
  }

  std::vector<IndexEntry> entries;
  std::unordered_map<std::string, int> token_counts;
  std::unordered_set<std::string> seen_recap_entries;

  auto can_add_token = [&](const std::string& token) -> bool {
    auto it = token_counts.find(token);
    if (it == token_counts.end()) return true;
    return it->second < opt.max_entries_per_token;
  };

  auto record_token = [&](const std::string& token) {
    token_counts[token]++;
  };

  if (opt.include_structured) {
    std::vector<MemoryCheckpointMeta> metas;
    std::string lerr;
    if (memory_list_structured_checkpoints(mem_root, 0, INT64_MAX, /*structured_path_filter=*/"", std::max(1, opt.max_structured_checkpoints), &metas, &lerr)) {
      int structured_added = 0;
      for (const auto& meta : metas) {
        std::string structured_path;
        Json::Value items(Json::nullValue);
        std::string rerr;
        if (!memory_read_structured_checkpoint_items(mem_root, meta.checkpoint_path_rel, &structured_path, &items, &rerr)) continue;
        if (!items.isObject()) continue;

        std::vector<std::string> keys = items.getMemberNames();
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys) {
          if (structured_added >= opt.max_structured_entries) break;
          const Json::Value rec = items[key];
          if (!rec.isObject()) continue;
          const Json::Value sources = rec.isMember("sources") ? rec["sources"] : Json::Value(Json::nullValue);
          if (!sources.isArray()) continue;

          std::vector<std::string> tokens;
          extract_tokens_from_sources(sources, &tokens);
          if (tokens.empty()) continue;

          Json::Value payload(Json::objectValue);
          payload["key"] = key;
          payload["record"] = rec;
          payload["structured_path"] = structured_path;
          payload["checkpoint_path"] = meta.checkpoint_path_rel;
          payload["checkpoint_ts_utc"] = meta.ts_utc;
          payload["checkpoint_ts_utc_ms"] = (Json::Int64)meta.ts_utc_ms;

          for (const auto& tok : tokens) {
            if (!can_add_token(tok)) continue;
            IndexEntry entry;
            entry.token = tok;
            entry.kind = "structured";
            entry.ref = std::string("structured:") + key;
            entry.ts_ms = meta.ts_utc_ms;
            entry.payload = payload;
            entries.push_back(entry);
            structured_added++;
            rep.structured_entries++;
            record_token(tok);

            if (opt.include_recaps) {
              const std::string recap_key = entry.ref;
              auto it = recaps_by_source.find(recap_key);
              if (it != recaps_by_source.end()) {
                for (const auto& recap : it->second) {
                  const std::string dedupe_key = tok + "::" + recap.path_rel;
                  if (seen_recap_entries.count(dedupe_key)) continue;
                  seen_recap_entries.insert(dedupe_key);

                  IndexEntry re;
                  re.token = tok;
                  re.kind = "recap";
                  re.ref = recap.path_rel;
                  re.ts_ms = recap.ts_ms;
                  Json::Value rp(Json::objectValue);
                  rp["recap_path"] = recap.path_rel;
                  if (!recap.kind.empty()) rp["kind"] = recap.kind;
                  if (!recap.ts_utc.empty()) rp["ts_utc"] = recap.ts_utc;
                  if (recap.ts_ms > 0) rp["ts_utc_ms"] = (Json::Int64)recap.ts_ms;
                  if (!recap.summary_excerpt.empty()) rp["summary_excerpt"] = recap.summary_excerpt;
                  if (!recap.evidence_sources.empty()) {
                    Json::Value ev(Json::arrayValue);
                    for (const auto& s : recap.evidence_sources) ev.append(s);
                    rp["evidence_sources"] = ev;
                  }
                  re.payload = rp;
                  entries.push_back(re);
                  rep.recap_entries++;
                }
              }
            }
          }
        }
      }
    }
  }

  if (opt.include_daily) {
    const int days = std::max(0, opt.daily_days);
    int daily_added = 0;
    for (int i = 0; i < days; i++) {
      if (daily_added >= opt.max_daily_entries) break;
      const std::string ymd = local_date_ymd_days_ago(i);
      const std::filesystem::path p = mem_root / (ymd + ".md");
      std::error_code ec;
      if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) continue;

      std::vector<DailyObs> obs;
      parse_daily_obs_file(p, p.filename().string(), opt.max_daily_entries - daily_added, &obs);
      for (const auto& item : obs) {
        if (daily_added >= opt.max_daily_entries) break;
        std::vector<std::string> tokens;
        const std::string tok = std::string("trace:") + item.trace_id;
        if (!tok.empty()) tokens.push_back(tok);
        const size_t extra = item.trace_id.find(':');
        if (extra != std::string::npos && extra > 0) {
          tokens.push_back(std::string("trace:") + item.trace_id.substr(0, extra));
        }
        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());

        Json::Value payload(Json::objectValue);
        payload["path"] = item.path_rel;
        payload["line"] = item.line;
        if (!item.text.empty()) payload["text"] = truncate_ascii(item.text, opt.value_excerpt_chars);
        if (!item.trace_id.empty()) payload["trace_id"] = item.trace_id;
        if (!item.source.empty()) payload["source"] = item.source;
        if (!item.ts_utc.empty()) payload["ts_utc"] = item.ts_utc;
        if (item.importance >= 0) payload["importance"] = item.importance;

        std::ostringstream ref;
        ref << item.path_rel << "#L" << item.line;
        const std::string entry_ref = ref.str();

        int64_t ts_ms = 0;
        if (!item.ts_utc.empty()) {
          (void)parse_iso_utc_ms(item.ts_utc, &ts_ms);
        }

        for (const auto& t : tokens) {
          if (!can_add_token(t)) continue;
          IndexEntry entry;
          entry.token = t;
          entry.kind = "daily";
          entry.ref = entry_ref;
          entry.ts_ms = ts_ms;
          entry.payload = payload;
          entries.push_back(entry);
          daily_added++;
          rep.daily_entries++;
          record_token(t);

          if (opt.include_recaps) {
            const std::string recap_key = entry.ref;
            auto it = recaps_by_source.find(recap_key);
            if (it != recaps_by_source.end()) {
              for (const auto& recap : it->second) {
                const std::string dedupe_key = t + "::" + recap.path_rel;
                if (seen_recap_entries.count(dedupe_key)) continue;
                seen_recap_entries.insert(dedupe_key);

                IndexEntry re;
                re.token = t;
                re.kind = "recap";
                re.ref = recap.path_rel;
                re.ts_ms = recap.ts_ms;
                Json::Value rp(Json::objectValue);
                rp["recap_path"] = recap.path_rel;
                if (!recap.kind.empty()) rp["kind"] = recap.kind;
                if (!recap.ts_utc.empty()) rp["ts_utc"] = recap.ts_utc;
                if (recap.ts_ms > 0) rp["ts_utc_ms"] = (Json::Int64)recap.ts_ms;
                if (!recap.summary_excerpt.empty()) rp["summary_excerpt"] = recap.summary_excerpt;
                if (!recap.evidence_sources.empty()) {
                  Json::Value ev(Json::arrayValue);
                  for (const auto& s : recap.evidence_sources) ev.append(s);
                  rp["evidence_sources"] = ev;
                }
                re.payload = rp;
                entries.push_back(re);
                rep.recap_entries++;
              }
            }
          }
        }
      }
    }
  }

  rep.token_count = (int)token_counts.size();
  rep.entry_count = (int)entries.size();

#if defined(AGENT_HAVE_SQLITE3)
  rep.index_path = index_sqlite_path(mem_root).string();
  if (!sqlite_write_index(index_sqlite_path(mem_root), rep, entries, out_err)) {
    return false;
  }
#else
  rep.index_path = index_json_path(mem_root).string();
  if (!json_write_index(index_json_path(mem_root), rep, entries, out_err)) {
    return false;
  }
#endif

  if (out_report) *out_report = rep;
  return true;
}

bool memory_correlation_index_query(
  const std::filesystem::path& memory_root,
  const std::string& token,
  int max_entries,
  Json::Value* out_entries,
  Json::Value* out_meta,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_entries) *out_entries = Json::Value(Json::arrayValue);
  if (out_meta) *out_meta = Json::Value(Json::objectValue);

  if (memory_root.empty() || token.empty()) {
    if (out_err) *out_err = "invalid args";
    return false;
  }

  max_entries = std::max(1, std::min(2000, max_entries));

#if defined(AGENT_HAVE_SQLITE3)
  const std::filesystem::path path = index_sqlite_path(memory_root);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (out_err) *out_err = "correlation index missing";
    return false;
  }
  if (!sqlite_query_index(path, token, max_entries, out_entries, out_meta, out_err)) {
    return false;
  }
  if (out_meta) {
    (*out_meta)["index_path"] = path.string();
  }
  return true;
#else
  const std::filesystem::path path = index_json_path(memory_root);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (out_err) *out_err = "correlation index missing";
    return false;
  }
  if (!json_query_index(path, token, max_entries, out_entries, out_meta, out_err)) {
    return false;
  }
  if (out_meta) {
    (*out_meta)["index_path"] = path.string();
  }
  return true;
#endif
}

}  // namespace agentd
