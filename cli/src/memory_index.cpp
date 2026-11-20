#include "memory_index.h"
#if defined(AGENT_HAVE_JSONCPP)
#include "agent_sha256.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <sstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif
namespace host_tools_internal {
namespace {
std::string lower(std::string text) {
  for (auto &c : text)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return text;
}
void append(const MemoryPassage &passage, int cap, double score, std::vector<MemorySearchHit> *hits) {
  MemorySearchHit hit;
  hit.path = passage.path;
  hit.line = passage.line;
  hit.passage = passage;
  hit.snippet = memory_view_clip(passage.text, size_t(cap));
  hit.score = score;
  hits->push_back(std::move(hit));
}
#if defined(AGENT_HAVE_SQLITE3)
struct Database {
  sqlite3 *db = nullptr;
  ~Database() {
    if (db)
      sqlite3_close(db);
  }
};
struct Statement {
  sqlite3_stmt *value = nullptr;
  ~Statement() {
    if (value)
      sqlite3_finalize(value);
  }
};
bool sql(Database &db, const char *text, std::string *error) {
  if (sqlite3_exec(db.db, text, nullptr, nullptr, nullptr) == SQLITE_OK)
    return true;
  if (error)
    *error = sqlite3_errmsg(db.db);
  return false;
}
bool prepare(Database &db, const char *text, Statement *statement, std::string *error) {
  if (sqlite3_prepare_v2(db.db, text, -1, &statement->value, nullptr) == SQLITE_OK)
    return true;
  if (error)
    *error = sqlite3_errmsg(db.db);
  return false;
}
std::string signature(const MemoryViewSnapshot &snapshot) {
  agent_sha256_ctx_t state;
  agent_sha256_init(&state);
  const auto feed = [&](const std::string &value) {
    uint8_t length[8]{};
    for (int i = 0; i < 8; ++i)
      length[7 - i] = uint8_t(uint64_t(value.size()) >> (i * 8));
    agent_sha256_update(&state, length, sizeof(length));
    agent_sha256_update(&state, value.data(), value.size());
  };
  feed("memory-view-v2-secure-cache");
  feed(memory_view_name(snapshot.mode));
  for (const auto &source : snapshot.sources) {
    feed(source.first);
    feed(source.second.revision);
  }
  for (const auto &passage : snapshot.passages) {
    feed(memory_passage_evidence(passage)["id"].asString());
    feed(passage.text);
  }
  uint8_t raw[32]{};
  char hash[65]{};
  agent_sha256_final(&state, raw);
  agent_sha256_hex(raw, hash);
  return hash;
}
bool indexed(const MemoryViewSnapshot &snapshot, const std::string &query, int limit, int cap,
             std::vector<MemorySearchHit> *hits, std::string *error) {
  MemoryCacheLease cache_lease;
  if (cache_lease.begin(snapshot.root, {}, error) != MemoryStoreStatus::Ok ||
      !cache_lease.discard_obsolete(error) || !memory_view_validate(snapshot, error))
    return false;
  const auto path = snapshot.root / ".memory_retrieval_v2.sqlite3";
  std::error_code ec;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec))) {
    if (error)
      *error = "linked retrieval cache refused";
    return false;
  }
#if !defined(_WIN32)
  const int cache = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (cache < 0) {
    if (error)
      *error = "cannot create private retrieval cache";
    return false;
  }
  const bool private_cache = ::fchmod(cache, 0600) == 0;
  ::close(cache);
  if (!private_cache) {
    if (error)
      *error = "cannot restrict retrieval cache permissions";
    return false;
  }
#endif
  Database db;
  if (sqlite3_open_v2(path.string().c_str(), &db.db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (error)
      *error = "retrieval cache unavailable";
    return false;
  }
  sqlite3_busy_timeout(db.db, 500);
  if (sqlite3_libversion_number() < 3042000) {
    if (error)
      *error = "FTS secure-delete requires SQLite 3.42 or newer";
    return false;
  }
  if (!sql(db,
           "PRAGMA journal_mode=DELETE;PRAGMA secure_delete=1;PRAGMA temp_store=MEMORY;"
           "CREATE TABLE IF NOT EXISTS view_meta(signature TEXT NOT NULL);"
           "CREATE VIRTUAL TABLE IF NOT EXISTS eligible_passages USING fts5(content,evidence_id "
           "UNINDEXED,tokenize='unicode61');"
           "INSERT INTO eligible_passages(eligible_passages,rank) VALUES('secure-delete',1);"
           "BEGIN IMMEDIATE;",
           error))
    return false;
  Statement secure;
  if (!prepare(db, "PRAGMA secure_delete;", &secure, error) ||
      sqlite3_step(secure.value) != SQLITE_ROW || sqlite3_column_int(secure.value, 0) != 1) {
    if (error)
      *error = "SQLite secure_delete was not enabled";
    return false;
  }
  bool committed = false;
  struct Rollback {
    Database &db;
    bool &committed;
    ~Rollback() {
      if (!committed)
        sqlite3_exec(db.db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  } rollback{db, committed};
  const auto expected = signature(snapshot);
  bool same = false;
  {
    Statement old;
    if (!prepare(db, "SELECT signature FROM view_meta LIMIT 1;", &old, error))
      return false;
    if (sqlite3_step(old.value) == SQLITE_ROW) {
      const auto value = reinterpret_cast<const char *>(sqlite3_column_text(old.value, 0));
      same = value && expected == value;
    }
  }
  if (!same) {
    if (!sql(db, "DELETE FROM eligible_passages;DELETE FROM view_meta;", error))
      return false;
    Statement insert, meta;
    if (!prepare(db, "INSERT INTO eligible_passages(content,evidence_id) VALUES(?1,?2);", &insert, error) ||
        !prepare(db, "INSERT INTO view_meta VALUES(?1);", &meta, error))
      return false;
    for (const auto &passage : snapshot.passages) {
      const auto id = memory_passage_evidence(passage)["id"].asString();
      sqlite3_reset(insert.value);
      sqlite3_clear_bindings(insert.value);
      sqlite3_bind_text(insert.value, 1, passage.text.c_str(), int(passage.text.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(insert.value, 2, id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(insert.value) != SQLITE_DONE) {
        if (error)
          *error = sqlite3_errmsg(db.db);
        return false;
      }
    }
    sqlite3_bind_text(meta.value, 1, expected.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(meta.value) != SQLITE_DONE)
      return false;
  }
  std::map<std::string, const MemoryPassage *> eligible;
  for (const auto &passage : snapshot.passages)
    eligible.emplace(memory_passage_evidence(passage)["id"].asString(), &passage);
  Statement query_statement;
  if (!prepare(db,
               "SELECT evidence_id,bm25(eligible_passages) FROM eligible_passages WHERE eligible_passages "
               "MATCH ?1 ORDER BY bm25(eligible_passages),rowid LIMIT ?2;",
               &query_statement, error))
    return false;
  auto run = [&](const std::string &input) {
    hits->clear();
    sqlite3_reset(query_statement.value);
    sqlite3_clear_bindings(query_statement.value);
    sqlite3_bind_text(query_statement.value, 1, input.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(query_statement.value, 2, limit);
    int rc;
    while ((rc = sqlite3_step(query_statement.value)) == SQLITE_ROW) {
      const auto id = reinterpret_cast<const char *>(sqlite3_column_text(query_statement.value, 0));
      const auto score = -sqlite3_column_double(query_statement.value, 1);
      auto it = eligible.find(id ? id : "");
      if (it == eligible.end() || !std::isfinite(score)) {
        if (error)
          *error = "retrieval cache contains an ineligible evidence ID";
        return false;
      }
      append(*it->second, cap, score, hits);
    }
    return rc == SQLITE_DONE;
  };
  if (!run(query)) {
    // Quoted tokens preserve Chinese and code identifiers without interpreting
    // punctuation as FTS operators. No model call is needed for this fallback.
    std::string token, sanitized;
    auto flush = [&] {
      if (!token.empty()) {
        if (!sanitized.empty())
          sanitized += ' ';
        sanitized += '"' + token + '"';
        token.clear();
      }
    };
    for (const unsigned char c : query) {
      if (c >= 128 || std::isalnum(c) || c == '_' || c == '.' || c == '-')
        token += char(c);
      else
        flush();
    }
    flush();
    if (sanitized.empty() || !run(sanitized)) {
      if (error)
        *error = sqlite3_errmsg(db.db);
      hits->clear();
      return false;
    }
  }
  if (!sql(db, "COMMIT;", error)) {
    hits->clear();
    return false;
  }
  committed = true;
  return true;
}
#else
bool indexed(const MemoryViewSnapshot &, const std::string &, int, int, std::vector<MemorySearchHit> *,
             std::string *error) {
  if (error)
    *error = "sqlite3 not enabled";
  return false;
}
#endif
} // namespace
bool memory_retrieve_snapshot(const MemoryViewSnapshot &snapshot, const std::string &query, bool use_index,
                              bool case_sensitive, int max_results, int max_snippet_chars,
                              std::vector<MemorySearchHit> *hits, std::string *engine, std::string *error,
                              std::string *index_error, int context_lines) {
  if (!hits || !engine)
    return false;
  hits->clear();
  if (error)
    error->clear();
  if (index_error)
    index_error->clear();
  if (query.empty() || query.size() > 4096) {
    if (error)
      *error = "query must contain 1..4096 bytes";
    return false;
  }
  const auto limit = std::clamp(max_results, 1, 200), cap = std::clamp(max_snippet_chars, 80, 4000);
  if (!memory_view_validate(snapshot, error))
    return false;
  *engine = "substr";
  std::string failure;
  if (use_index && !case_sensitive && indexed(snapshot, query, limit, cap, hits, &failure))
    *engine = "fts5";
  else if (!failure.empty() && index_error)
    *index_error = failure;
  if (*engine != "fts5" || hits->empty()) {
    hits->clear();
    *engine = "substr";
    const auto needle = case_sensitive ? query : lower(query);
    for (const auto &passage : snapshot.passages) {
      if ((case_sensitive ? passage.text : lower(passage.text)).find(needle) == std::string::npos)
        continue;
      append(passage, cap, 0, hits);
      if (int(hits->size()) == limit)
        break;
    }
  }
  for (auto &hit : *hits) {
    const auto hay = case_sensitive ? hit.passage.text : lower(hit.passage.text);
    const auto needle = case_sensitive ? query : lower(query);
    const auto match = hay.find(needle);
    if (match == std::string::npos)
      continue;
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < hay.size(); ++i)
      if (hay[i] == '\n')
        starts.push_back(i + 1);
    const auto line = size_t(std::upper_bound(starts.begin(), starts.end(), match) - starts.begin() - 1);
    const auto neighbors = size_t(std::clamp(context_lines, 0, 20));
    size_t begin = starts[line > neighbors ? line - neighbors : 0];
    const auto after = std::min(starts.size(), line + neighbors + 1);
    const auto end = after == starts.size() ? hay.size() : starts[after];
    if (match - begin >= size_t(cap))
      begin = match - size_t(cap) / 2;
    while (begin < hay.size() && (static_cast<unsigned char>(hit.passage.text[begin]) & 0xc0) == 0x80)
      ++begin;
    hit.snippet = memory_view_clip(hit.passage.text.substr(begin, end - begin), size_t(cap));
    if (hit.passage.pointer.empty())
      hit.line = hit.passage.line + int(line);
  }
  if (!memory_view_validate(snapshot, error)) {
    hits->clear();
    return false;
  }
  return true;
}
bool memory_retrieve(const std::filesystem::path &root, const std::vector<std::string> &files,
                     const std::string &query, MemoryViewMode mode, bool use_index, bool case_sensitive,
                     int max_results, int max_snippet_chars, std::vector<MemorySearchHit> *hits,
                     std::string *engine, std::string *error, std::string *index_error, int context_lines) {
  if (hits)
    hits->clear();
  MemoryViewSnapshot snapshot;
  if (!memory_view_capture(root, files, mode, &snapshot, error))
    return false;
  return memory_retrieve_snapshot(snapshot, query, use_index, case_sensitive, max_results, max_snippet_chars,
                                  hits, engine, error, index_error, context_lines);
}
bool memory_index_search_ranked(const std::filesystem::path &root, const std::vector<std::string> &files,
                                const std::string &query, int max_results, int max_snippet_chars,
                                std::vector<MemorySearchHit> *hits, std::string *error) {
  std::string engine;
  return memory_retrieve(root, files, query, MemoryViewMode::Current, true, false, max_results,
                         max_snippet_chars, hits, &engine, error);
}
} // namespace host_tools_internal
#else
namespace host_tools_internal {
bool memory_index_search_ranked(const std::filesystem::path &, const std::vector<std::string> &,
                                const std::string &, int, int, std::vector<MemorySearchHit> *,
                                std::string *error) {
  if (error)
    *error = "jsoncpp not enabled";
  return false;
}
} // namespace host_tools_internal
#endif
