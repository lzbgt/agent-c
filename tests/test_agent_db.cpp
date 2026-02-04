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

static void exec_sql(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::fprintf(stderr, "exec failed: %s\n", err ? err : sqlite3_errmsg(db));
    if (err) sqlite3_free(err);
    std::abort();
  }
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

  agentd::AgentDb::ArtifactRow ar;
  ar.run_id = run_id;
  ar.ts_unix_ms = now;
  ar.session_id = "s1";
  ar.tool_call_id = "call_2";
  ar.path = "build/demo.wav";
  ar.kind = "audio";
  ar.mime = "audio/wav";
  ar.title = "demo clip";
  ar.autoplay = true;
  ar.repeat = 2;
  ar.artifact_json = "{\"path\":\"build/demo.wav\",\"kind\":\"audio\"}";
  if (!db.insert_artifact(ar, &err)) {
    std::fprintf(stderr, "insert_artifact failed: %s\n", err.c_str());
    return 1;
  }

  agentd::AgentDb::UiActionRow ur;
  ur.run_id = run_id;
  ur.ts_unix_ms = now;
  ur.session_id = "s1";
  ur.tool_call_id = "call_3";
  ur.type = "notify";
  ur.title = "hello";
  ur.message = "world";
  ur.autoplay = false;
  ur.repeat = 1;
  ur.action_json = "{\"type\":\"notify\",\"title\":\"hello\",\"message\":\"world\"}";
  if (!db.insert_ui_action(ur, &err)) {
    std::fprintf(stderr, "insert_ui_action failed: %s\n", err.c_str());
    return 1;
  }

  agentd::AgentDb::ClientEventRow cer;
  cer.ts_unix_ms = now;
  cer.session_id = "s1";
  cer.type = "artifact_rendered";
  cer.data_json = "{\"type\":\"artifact_rendered\",\"ts_unix_ms\":1700000000000,\"data\":{\"path\":\"build/demo.wav\"}}";
  if (!db.insert_client_event(cer, &err)) {
    std::fprintf(stderr, "insert_client_event failed: %s\n", err.c_str());
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
  const int64_t arts = query_i64(raw, "SELECT COUNT(*) FROM artifacts;");
  const int64_t uas = query_i64(raw, "SELECT COUNT(*) FROM ui_actions;");
  const int64_t ces = query_i64(raw, "SELECT COUNT(*) FROM client_events;");
  sqlite3_close(raw);

  assert(sessions == 1);
  assert(messages == 2);
  assert(runs == 1);
  assert(events == 1);
  assert(tools == 1);
  assert(arts == 1);
  assert(uas == 1);
  assert(ces == 1);

  // Migration smoke: open an older (v1) DB and ensure it upgrades to the latest schema.
  const std::filesystem::path tmp2 =
    std::filesystem::temp_directory_path() / ("agentd_db_migrate_test_" + std::to_string((long long)getpid()) + ".sqlite");
  std::filesystem::remove(tmp2, ec);
  {
    sqlite3* pre = nullptr;
    if (sqlite3_open_v2(tmp2.string().c_str(), &pre, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
      std::fprintf(stderr, "sqlite3_open_v2 precreate failed\n");
      return 1;
    }
    exec_sql(pre, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);");
    exec_sql(pre, "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','1');");
    sqlite3_close(pre);
  }

  agentd::AgentDb db2;
  if (!db2.open(tmp2.string(), &err)) {
    std::fprintf(stderr, "db2.open failed: %s\n", err.c_str());
    return 1;
  }
  db2.close();

  sqlite3* raw2 = nullptr;
  if (sqlite3_open_v2(tmp2.string().c_str(), &raw2, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    std::fprintf(stderr, "sqlite3_open_v2 raw2 failed\n");
    return 1;
  }
  const int64_t ver = query_i64(raw2, "SELECT CAST(value AS INTEGER) FROM meta WHERE key='schema_version' LIMIT 1;");
  const int64_t arts2 = query_i64(raw2, "SELECT COUNT(*) FROM artifacts;");
  const int64_t stop_reason_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('runs') WHERE name='stop_reason';");
  const int64_t ua_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('ui_actions') WHERE name='action_json';");
  const int64_t ce_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('client_events') WHERE name='data_json';");
  const int64_t audit_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('audit_records') WHERE name='record_json';");
  const int64_t scene_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('scene_states') WHERE name='scene_json';");
  const int64_t job_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('jobs') WHERE name='job_id';");
  const int64_t job_trace_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('jobs') WHERE name='trace_id';");
  const int64_t job_req_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('jobs') WHERE name='request_json';");
  const int64_t workflow_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflows') WHERE name='workflow_id';");
  const int64_t workflow_task_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflow_tasks') WHERE name='task_id';");
  const int64_t workflow_event_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflow_events') WHERE name='event_id';");
  const int64_t workflow_event_type_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflow_events') WHERE name='type';");
  const int64_t job_prio_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('jobs') WHERE name='priority';");
  const int64_t workflow_prio_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflows') WHERE name='priority';");
  const int64_t workflow_task_prio_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('workflow_tasks') WHERE name='priority';");
  const int64_t edge_nodes_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_nodes') WHERE name='node_id';");
  const int64_t edge_outbox_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_outbox_messages') WHERE name='outbox_id';");
  const int64_t edge_inbox_msg_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_inbox_messages') WHERE name='msg_id';");
  const int64_t edge_task_task_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_tasks') WHERE name='task_id';");
  const int64_t edge_task_tool_name_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_tasks') WHERE name='tool_name';");
  const int64_t edge_task_event_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_task_events') WHERE name='id';");
  const int64_t edge_sensor_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_sensor_events') WHERE name='id';");
  const int64_t edge_rate_node_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_tool_rate_state') WHERE name='node_id';");
  const int64_t edge_rules_rule_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_rules') WHERE name='rule_id';");
  const int64_t edge_workflows_wid_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflows') WHERE name='workflow_id';");
  const int64_t edge_wf_steps_step_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_steps') WHERE name='step_id';");
  const int64_t edge_wf_steps_attempt_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_steps') WHERE name='attempt';");
  const int64_t edge_wf_steps_max_attempts_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_steps') WHERE name='max_attempts';");
  const int64_t edge_wf_steps_next_ready_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_steps') WHERE name='next_ready_utc_ms';");
  const int64_t edge_wf_steps_backoff_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_steps') WHERE name='backoff_ms';");
  const int64_t edge_wf_events_id_cols =
    query_i64(raw2, "SELECT COUNT(*) FROM pragma_table_info('edge_workflow_events') WHERE name='id';");
  sqlite3_close(raw2);
  assert(ver == 15);
  assert(arts2 == 0);
  assert(stop_reason_cols == 1);
  assert(ua_cols == 1);
  assert(ce_cols == 1);
  assert(audit_cols == 1);
  assert(scene_cols == 1);
  assert(job_id_cols == 1);
  assert(job_trace_cols == 1);
  assert(job_req_cols == 1);
  assert(workflow_id_cols == 1);
  assert(workflow_task_id_cols == 1);
  assert(workflow_event_id_cols == 1);
  assert(workflow_event_type_cols == 1);
  assert(job_prio_cols == 1);
  assert(workflow_prio_cols == 1);
  assert(workflow_task_prio_cols == 1);
  assert(edge_nodes_id_cols == 1);
  assert(edge_outbox_id_cols == 1);
  assert(edge_inbox_msg_id_cols == 1);
  assert(edge_task_task_id_cols == 1);
  assert(edge_task_tool_name_cols == 1);
  assert(edge_task_event_id_cols == 1);
  assert(edge_sensor_id_cols == 1);
  assert(edge_rate_node_cols == 1);
  assert(edge_rules_rule_id_cols == 1);
  assert(edge_workflows_wid_cols == 1);
  assert(edge_wf_steps_step_id_cols == 1);
  assert(edge_wf_steps_attempt_cols == 1);
  assert(edge_wf_steps_max_attempts_cols == 1);
  assert(edge_wf_steps_next_ready_cols == 1);
  assert(edge_wf_steps_backoff_cols == 1);
  assert(edge_wf_events_id_cols == 1);

  db.close();
  std::filesystem::remove(tmp, ec);
  std::filesystem::remove(tmp2, ec);
  return 0;
#endif
}
