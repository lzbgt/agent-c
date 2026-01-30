#include "agent_db.h"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

static int64_t query_i64(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(db));
    std::abort();
  }
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    std::fprintf(stderr, "step failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    std::abort();
  }
  const int64_t v = (int64_t)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

int main() {
#if !defined(AGENT_HAVE_SQLITE3)
  // Build should generally enable sqlite on desktop, but allow skipping on minimal environments.
  return 77;
#else
  const std::filesystem::path tmp =
    std::filesystem::temp_directory_path() / ("agentd_db_test_" + std::to_string((long long)getpid()) + ".sqlite");
  std::error_code ec;
  std::filesystem::remove(tmp, ec);

  agentd::AgentDb db;
  std::string err;
  if (!db.open(tmp.string(), &err)) {
    std::fprintf(stderr, "db.open failed: %s\n", err.c_str());
    return 1;
  }

  const int64_t now = 1700000000000LL;
  std::vector<std::pair<std::string, std::string>> msgs = {
    {"user", "hi"},
    {"assistant", "ok"},
  };
  if (!db.replace_session_messages("s1", msgs, now, &err)) {
    std::fprintf(stderr, "replace_session_messages failed: %s\n", err.c_str());
    return 1;
  }

  agentd::AgentDb::RunRow rr;
  rr.session_id = "s1";
  rr.ts_unix_ms = now;
  rr.prompt = "hello";
  rr.tools = "host";
  rr.model = "stub";
  rr.base_url = "http://stub";
  rr.stream_assistant = false;
  rr.ok = true;
  rr.http_status = 200;
  int64_t run_id = 0;
  if (!db.insert_run(rr, &run_id, &err) || run_id <= 0) {
    std::fprintf(stderr, "insert_run failed: %s run_id=%lld\n", err.c_str(), (long long)run_id);
    return 1;
  }

  if (!db.insert_event(run_id, now, "start", "{\"x\":1}", &err)) {
    std::fprintf(stderr, "insert_event failed: %s\n", err.c_str());
    return 1;
  }

  agentd::AgentDb::ToolRecordRow tr;
  tr.run_id = run_id;
  tr.tool_name = "fs_read";
  tr.tool_call_id = "call_1";
  tr.arguments_json = "{\"path\":\"README.md\"}";
  tr.result_text = "{\"ok\":true}";
  tr.result_for_prompt_text = "{\"ok\":true}";
  tr.result_truncated_for_prompt = false;
  if (!db.insert_tool_record(tr, &err)) {
    std::fprintf(stderr, "insert_tool_record failed: %s\n", err.c_str());
    return 1;
  }

  // Open a second handle and verify row counts.
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(tmp.string().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "sqlite3_open_v2 failed\n");
    return 1;
  }
  const int64_t sessions = query_i64(raw, "SELECT COUNT(*) FROM sessions;");
  const int64_t messages = query_i64(raw, "SELECT COUNT(*) FROM messages;");
  const int64_t runs = query_i64(raw, "SELECT COUNT(*) FROM runs;");
  const int64_t events = query_i64(raw, "SELECT COUNT(*) FROM events;");
  const int64_t tools = query_i64(raw, "SELECT COUNT(*) FROM tool_records;");
  sqlite3_close(raw);

  assert(sessions == 1);
  assert(messages == 2);
  assert(runs == 1);
  assert(events == 1);
  assert(tools == 1);

  db.close();
  std::filesystem::remove(tmp, ec);
  return 0;
#endif
}
