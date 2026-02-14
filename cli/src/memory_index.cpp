#include "memory_index.h"

#include "toolset_host_internal.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace host_tools_internal {

static int64_t unix_ms_now_() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string to_lower_ascii_(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static std::string trim_ascii_(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::string normalize_rel_md_path_(const std::filesystem::path& mem_root, const std::filesystem::path& abs) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, mem_root, ec);
  if (ec) rel = abs.lexically_relative(mem_root);
  return to_generic_string(rel.lexically_normal());
}

static bool read_file_bounded_(const std::filesystem::path& p, size_t max_bytes, std::string* out) {
  if (!out) return false;
  out->clear();
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  if (max_bytes == 0) return true;
  out->reserve(std::min<size_t>(max_bytes, 1024 * 1024));
  std::vector<char> buf;
  buf.resize(std::min<size_t>(max_bytes, 64 * 1024));
  size_t remaining = max_bytes;
  while (remaining > 0 && in.good()) {
    const size_t want = std::min(buf.size(), remaining);
    in.read(buf.data(), (std::streamsize)want);
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    out->append(buf.data(), (size_t)got);
    remaining -= (size_t)got;
  }
  return true;
}

struct Chunk {
  std::string text;
  int start_line = 1;
};

static bool line_is_marker_(const std::string& line, const char* marker) {
  if (!marker) return false;
  return line.find(marker) != std::string::npos;
}

static std::vector<Chunk> chunk_markdown_for_index_(const std::string& content, int max_lines, int max_chars_per_chunk) {
  std::vector<Chunk> out;
  out.reserve(128);

  std::vector<std::string> lines;
  lines.reserve(2048);
  {
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
      if ((int)lines.size() >= max_lines) break;
    }
  }

  bool skip_json_block = false;
  Chunk cur;
  cur.start_line = 1;

  auto flush = [&]() {
    const std::string t = trim_ascii_(cur.text);
    if (!t.empty()) {
      Chunk c;
      c.text = t;
      c.start_line = cur.start_line;
      out.push_back(std::move(c));
    }
    cur.text.clear();
  };

  for (size_t i = 0; i < lines.size(); i++) {
    const std::string& ln = lines[i];
    const int line_no = (int)i + 1;

    // Avoid indexing the machine JSON block embedded in STRUCTURED.md.
    if (line_is_marker_(ln, "<!-- AGENT_MEMORY_V1_BEGIN -->")) {
      skip_json_block = true;
      continue;
    }
    if (skip_json_block) {
      if (line_is_marker_(ln, "<!-- AGENT_MEMORY_V1_END -->")) {
        skip_json_block = false;
      }
      continue;
    }

    // Create a new chunk at headings and blank-line boundaries.
    const bool heading = (!ln.empty() && ln[0] == '#');
    const bool blank = trim_ascii_(ln).empty();

    if (cur.text.empty()) cur.start_line = line_no;
    const size_t projected = cur.text.size() + ln.size() + 1;
    if ((!cur.text.empty() && (heading || blank)) || (int)projected >= max_chars_per_chunk) {
      flush();
      cur.start_line = line_no;
    }

    // Keep blank lines as separators inside chunk; they help phrase scoring.
    cur.text.append(ln);
    cur.text.push_back('\n');
  }
  flush();

  // Bound total chunks (defense against pathological files).
  if (out.size() > 1000) out.resize(1000);
  return out;
}

#if !defined(AGENT_HAVE_SQLITE3)

bool memory_index_search_ranked(
  const std::filesystem::path&,
  const std::vector<std::string>&,
  const std::string&,
  int,
  int,
  std::vector<MemorySearchHit>*,
  std::string* out_err
) {
  if (out_err) *out_err = "sqlite3 not enabled";
  return false;
}

#else

struct SqliteDb {
  sqlite3* db = nullptr;
  ~SqliteDb() {
    if (db) sqlite3_close(db);
  }
};

struct SqliteStmt {
  sqlite3_stmt* st = nullptr;
  ~SqliteStmt() {
    if (st) sqlite3_finalize(st);
  }
};

static bool sqlite_exec_(sqlite3* db, const char* sql, std::string* out_err) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    if (out_err) {
      *out_err = err ? err : "sqlite exec failed";
    }
    sqlite3_free(err);
    return false;
  }
  return true;
}

static bool sqlite_prepare_(sqlite3* db, const char* sql, SqliteStmt* out, std::string* out_err) {
  if (!out) return false;
  out->st = nullptr;
  const int rc = sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr);
  if (rc != SQLITE_OK) {
    if (out_err) *out_err = sqlite3_errmsg(db);
    return false;
  }
  return true;
}

static bool sqlite_step_done_(sqlite3* db, sqlite3_stmt* st, std::string* out_err) {
  const int rc = sqlite3_step(st);
  if (rc == SQLITE_DONE) return true;
  if (out_err) *out_err = sqlite3_errmsg(db);
  return false;
}

static bool ensure_schema_(sqlite3* db, std::string* out_err) {
  if (!sqlite_exec_(db, "PRAGMA busy_timeout=5000;", out_err)) return false;
  sqlite_exec_(db, "PRAGMA journal_mode=WAL;", nullptr);

  if (!sqlite_exec_(
        db,
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS memory_files("
        "  path TEXT PRIMARY KEY,"
        "  mtime_unix_ms INTEGER NOT NULL,"
        "  size_bytes INTEGER NOT NULL,"
        "  indexed_unix_ms INTEGER NOT NULL"
        ");",
        out_err
      )) {
    return false;
  }

  // FTS5 may not be compiled into sqlite; treat as optional at runtime.
  if (!sqlite_exec_(
        db,
        "CREATE VIRTUAL TABLE IF NOT EXISTS memory_chunks USING fts5("
        "  content,"
        "  path UNINDEXED,"
        "  start_line UNINDEXED,"
        "  mtime_unix_ms UNINDEXED,"
        "  tokenize='unicode61'"
        ");",
        out_err
      )) {
    return false;
  }
  return true;
}

static bool file_mtime_unix_ms_(const std::filesystem::path& p, int64_t* out_mtime_ms, int64_t* out_size_bytes) {
  if (out_mtime_ms) *out_mtime_ms = 0;
  if (out_size_bytes) *out_size_bytes = 0;
  std::error_code ec;
  const auto st = std::filesystem::status(p, ec);
  if (ec || !std::filesystem::is_regular_file(st)) return false;
  const auto ft = std::filesystem::last_write_time(p, ec);
  if (!ec && out_mtime_ms) *out_mtime_ms = file_time_to_unix_ms(ft);
  const auto sz = std::filesystem::file_size(p, ec);
  if (!ec && out_size_bytes) *out_size_bytes = (int64_t)sz;
  return true;
}

static bool index_one_file_(
  sqlite3* db,
  const std::filesystem::path& mem_root,
  const std::filesystem::path& abs,
  const std::string& rel,
  int64_t mtime_ms,
  int64_t size_bytes,
  std::string* out_err
) {
  std::string content;
  if (!read_file_bounded_(abs, 2 * 1024 * 1024, &content)) {
    // Treat unreadable as not-indexed; delete any existing rows.
    SqliteStmt del1, del2;
    if (!sqlite_prepare_(db, "DELETE FROM memory_chunks WHERE path=?1;", &del1, out_err)) return false;
    sqlite3_bind_text(del1.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    if (!sqlite_step_done_(db, del1.st, out_err)) return false;
    if (!sqlite_prepare_(db, "DELETE FROM memory_files WHERE path=?1;", &del2, out_err)) return false;
    sqlite3_bind_text(del2.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    if (!sqlite_step_done_(db, del2.st, out_err)) return false;
    return true;
  }

  const auto chunks = chunk_markdown_for_index_(content, /*max_lines*/ 20000, /*max_chars_per_chunk*/ 1600);

  if (!sqlite_exec_(db, "BEGIN IMMEDIATE TRANSACTION;", out_err)) return false;
  bool ok = false;
  do {
    SqliteStmt del1, del2;
    if (!sqlite_prepare_(db, "DELETE FROM memory_chunks WHERE path=?1;", &del1, out_err)) break;
    sqlite3_bind_text(del1.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    if (!sqlite_step_done_(db, del1.st, out_err)) break;

    if (!sqlite_prepare_(db, "DELETE FROM memory_files WHERE path=?1;", &del2, out_err)) break;
    sqlite3_bind_text(del2.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    if (!sqlite_step_done_(db, del2.st, out_err)) break;

    SqliteStmt insf;
    if (!sqlite_prepare_(
          db,
          "INSERT INTO memory_files(path,mtime_unix_ms,size_bytes,indexed_unix_ms) VALUES(?1,?2,?3,?4);",
          &insf,
          out_err
        )) {
      break;
    }
    sqlite3_bind_text(insf.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insf.st, 2, (sqlite3_int64)mtime_ms);
    sqlite3_bind_int64(insf.st, 3, (sqlite3_int64)size_bytes);
    sqlite3_bind_int64(insf.st, 4, (sqlite3_int64)unix_ms_now_());
    if (!sqlite_step_done_(db, insf.st, out_err)) break;

    SqliteStmt insc;
    if (!sqlite_prepare_(
          db,
          "INSERT INTO memory_chunks(content,path,start_line,mtime_unix_ms) VALUES(?1,?2,?3,?4);",
          &insc,
          out_err
        )) {
      break;
    }
    for (const auto& c : chunks) {
      sqlite3_reset(insc.st);
      sqlite3_clear_bindings(insc.st);
      sqlite3_bind_text(insc.st, 1, c.text.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insc.st, 2, rel.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(insc.st, 3, c.start_line);
      sqlite3_bind_int64(insc.st, 4, (sqlite3_int64)mtime_ms);
      if (!sqlite_step_done_(db, insc.st, out_err)) {
        ok = false;
        break;
      }
      ok = true;
    }
    if (!ok && chunks.empty()) ok = true;
  } while (false);

  if (ok) {
    sqlite_exec_(db, "COMMIT;", nullptr);
    return true;
  }
  sqlite_exec_(db, "ROLLBACK;", nullptr);
  return false;
}

static std::string sanitize_fts_query_(const std::string& q) {
  // Keep it simple: extract alnum tokens and join with spaces (FTS default is AND).
  std::string out;
  out.reserve(q.size());
  std::string tok;
  auto flush_tok = [&]() {
    if (tok.size() < 2) {
      tok.clear();
      return;
    }
    if (!out.empty()) out.push_back(' ');
    out.append(tok);
    tok.clear();
  };
  for (char c : q) {
    const bool ok =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (ok) tok.push_back(c);
    else flush_tok();
    if (out.size() > 512) break;
  }
  flush_tok();
  return trim_ascii_(out);
}

static bool refresh_index_(
  sqlite3* db,
  const std::filesystem::path& mem_root,
  const std::vector<std::string>& allowed_abs,
  std::vector<std::string>* out_allowed_rel,
  std::string* out_err
) {
  if (out_allowed_rel) out_allowed_rel->clear();

  SqliteStmt sel;
  if (!sqlite_prepare_(db, "SELECT mtime_unix_ms FROM memory_files WHERE path=?1;", &sel, out_err)) return false;

  for (const auto& abs_s : allowed_abs) {
    std::filesystem::path abs(abs_s);
    int64_t mtime_ms = 0;
    int64_t sz = 0;
    if (!file_mtime_unix_ms_(abs, &mtime_ms, &sz)) continue;
    const std::string rel = normalize_rel_md_path_(mem_root, abs);
    if (out_allowed_rel) out_allowed_rel->push_back(rel);

    sqlite3_reset(sel.st);
    sqlite3_clear_bindings(sel.st);
    sqlite3_bind_text(sel.st, 1, rel.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(sel.st);
    if (rc == SQLITE_ROW) {
      const int64_t old_mtime = (int64_t)sqlite3_column_int64(sel.st, 0);
      if (old_mtime == mtime_ms) continue;
    } else if (rc != SQLITE_DONE) {
      if (out_err) *out_err = sqlite3_errmsg(db);
      return false;
    }

    if (!index_one_file_(db, mem_root, abs, rel, mtime_ms, sz, out_err)) {
      return false;
    }
  }

  // Stable ordering for deterministic output.
  if (out_allowed_rel) {
    std::sort(out_allowed_rel->begin(), out_allowed_rel->end());
    out_allowed_rel->erase(std::unique(out_allowed_rel->begin(), out_allowed_rel->end()), out_allowed_rel->end());
  }
  return true;
}

bool memory_index_search_ranked(
  const std::filesystem::path& mem_root,
  const std::vector<std::string>& allowed_files_abs,
  const std::string& query,
  int max_results,
  int max_snippet_chars,
  std::vector<MemorySearchHit>* out_hits,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_hits) out_hits->clear();
  if (!out_hits) return false;

  const std::filesystem::path db_path = mem_root / ".memory_index.sqlite3";
  std::error_code ec;
  std::filesystem::create_directories(mem_root, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create memory root for index";
    return false;
  }

  SqliteDb h;
  if (sqlite3_open(db_path.string().c_str(), &h.db) != SQLITE_OK) {
    if (out_err) *out_err = "failed to open sqlite db";
    return false;
  }

  std::string err;
  if (!ensure_schema_(h.db, &err)) {
    if (out_err) *out_err = err.empty() ? "failed to ensure schema" : err;
    return false;
  }

  std::vector<std::string> allowed_rel;
  if (!refresh_index_(h.db, mem_root, allowed_files_abs, &allowed_rel, &err)) {
    if (out_err) *out_err = err.empty() ? "failed to refresh index" : err;
    return false;
  }

  if (allowed_rel.empty()) {
    if (out_err) *out_err = "no memory files available";
    return true;
  }

  // Build a scoped query: restrict to allowed paths (daily_days etc).
  std::ostringstream sql;
  sql << "SELECT m.path, m.start_line, "
         "snippet(m, 0, '', '', ' … ', 12) AS snip, "
         "bm25(m) AS bm "
         "FROM memory_chunks AS m "
         "WHERE m MATCH ?1 AND m.path IN (";
  for (size_t i = 0; i < allowed_rel.size(); i++) {
    if (i) sql << ",";
    sql << "?" << (int)(2 + i);
  }
  sql << ") ORDER BY bm ASC LIMIT ?" << (int)(2 + allowed_rel.size()) << ";";

  SqliteStmt st;
  if (!sqlite_prepare_(h.db, sql.str().c_str(), &st, &err)) {
    if (out_err) *out_err = err.empty() ? "failed to prepare query" : err;
    return false;
  }

  // First try raw query; if it errors, retry with a sanitized query.
  auto run_query = [&](const std::string& q) -> bool {
    sqlite3_reset(st.st);
    sqlite3_clear_bindings(st.st);
    sqlite3_bind_text(st.st, 1, q.c_str(), -1, SQLITE_TRANSIENT);
    for (size_t i = 0; i < allowed_rel.size(); i++) {
      sqlite3_bind_text(st.st, (int)(2 + i), allowed_rel[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st.st, (int)(2 + allowed_rel.size()), max_results);

    int rc = 0;
    while ((rc = sqlite3_step(st.st)) == SQLITE_ROW) {
      const char* p = (const char*)sqlite3_column_text(st.st, 0);
      const int line = sqlite3_column_int(st.st, 1);
      const char* sn = (const char*)sqlite3_column_text(st.st, 2);
      const double bm = sqlite3_column_double(st.st, 3);
      MemorySearchHit hit;
      hit.path = p ? p : "";
      hit.line = std::max(1, line);
      hit.snippet = sn ? sn : "";
      hit.score = -bm; // smaller bm25 => better; expose higher-is-better
      if ((int)hit.snippet.size() > max_snippet_chars) hit.snippet.resize((size_t)max_snippet_chars);
      out_hits->push_back(std::move(hit));
      if ((int)out_hits->size() >= max_results) break;
    }
    if (rc == SQLITE_DONE) return true;
    err = sqlite3_errmsg(h.db);
    return false;
  };

  if (run_query(query)) return true;

  const std::string q2 = sanitize_fts_query_(query);
  if (q2.empty()) {
    if (out_err) *out_err = err.empty() ? "fts query failed" : err;
    return false;
  }
  if (!run_query(q2)) {
    if (out_err) *out_err = err.empty() ? "fts query failed" : err;
    return false;
  }
  return true;
}

#endif

} // namespace host_tools_internal
