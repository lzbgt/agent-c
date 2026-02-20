#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace agentd {

bool AgentDb::upsert_job(const JobRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.job_id.empty()) {
    if (out_error) *out_error = "upsert_job: job_id is empty";
    return false;
  }
  const int64_t now = (row.updated_unix_ms > 0) ? row.updated_unix_ms : agent_db_unix_ms_now();
  const int64_t created = (row.created_unix_ms > 0) ? row.created_unix_ms : now;
  const std::string status = row.status.empty() ? "queued" : row.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO jobs(
  job_id, session_id, trace_id, request_json, priority, created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(job_id) DO UPDATE SET
  session_id = CASE
    WHEN excluded.session_id IS NOT NULL AND excluded.session_id <> '' THEN excluded.session_id
    ELSE jobs.session_id
  END,
  trace_id = CASE
    WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id
    ELSE jobs.trace_id
  END,
  request_json = CASE
    WHEN excluded.request_json IS NOT NULL AND excluded.request_json <> '' THEN excluded.request_json
    ELSE jobs.request_json
  END,
  priority = CASE
    WHEN excluded.priority IS NOT NULL THEN excluded.priority
    ELSE jobs.priority
  END,
  updated_unix_ms = excluded.updated_unix_ms,
  status = CASE
    WHEN jobs.status IN ('done','error','cancelled','interrupted') THEN jobs.status
    ELSE excluded.status
  END,
  cancel_requested = CASE
    WHEN jobs.cancel_requested = 1 OR excluded.cancel_requested = 1 THEN 1
    ELSE 0
  END,
  error = CASE
    WHEN excluded.error IS NOT NULL AND excluded.error <> '' THEN excluded.error
    ELSE jobs.error
  END,
  stop_reason = CASE
    WHEN excluded.stop_reason IS NOT NULL AND excluded.stop_reason <> '' THEN excluded.stop_reason
    ELSE jobs.stop_reason
  END,
  result_json = CASE
    WHEN excluded.result_json IS NOT NULL AND excluded.result_json <> '' THEN excluded.result_json
    ELSE jobs.result_json
  END,
  last_heartbeat_unix_ms = CASE
    WHEN excluded.last_heartbeat_unix_ms IS NOT NULL AND excluded.last_heartbeat_unix_ms > COALESCE(jobs.last_heartbeat_unix_ms,0)
      THEN excluded.last_heartbeat_unix_ms
    ELSE jobs.last_heartbeat_unix_ms
  END;
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.job_id);
  ok = ok && agent_db_bind_text_or_null(st, 2, row.session_id);
  ok = ok && agent_db_bind_text_or_null(st, 3, row.trace_id);
  ok = ok && agent_db_bind_text_or_null(st, 4, row.request_json);
  ok = ok && agent_db_bind_i32_or_null(st, 5, row.priority, AgentDb::kIntUnset);
  ok = ok && agent_db_bind_i64(st, 6, created);
  ok = ok && agent_db_bind_i64(st, 7, now);
  ok = ok && agent_db_bind_text(st, 8, status);
  ok = ok && agent_db_bind_i32(st, 9, row.cancel_requested ? 1 : 0);
  ok = ok && agent_db_bind_text_or_null(st, 10, row.error);
  ok = ok && agent_db_bind_text_or_null(st, 11, row.stop_reason);
  ok = ok && agent_db_bind_text_or_null(st, 12, row.result_json);
  ok = ok && agent_db_bind_i64(st, 13, row.last_heartbeat_unix_ms);

  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = agent_db_sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_job(const std::string& job_id, JobRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = JobRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "get_job: job_id is empty";
    return false;
  }
  if (!out_row) {
    if (out_error) *out_error = "get_job: out_row is null";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
		SELECT
		  job_id, session_id, trace_id, request_json, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
		FROM jobs
		WHERE job_id=?
		LIMIT 1;
		)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, job_id);
  bool found = false;
	  if (ok && agent_db_step_row(st)) {
	    found = true;
	    const unsigned char* jid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* rq = sqlite3_column_text(st, 3);
	    out_row->job_id = jid ? (const char*)jid : "";
	    out_row->session_id = sid ? (const char*)sid : "";
	    out_row->trace_id = tid ? (const char*)tid : "";
	    out_row->request_json = rq ? (const char*)rq : "";
	    out_row->priority = sqlite3_column_int(st, 4);
	    out_row->created_unix_ms = sqlite3_column_int64(st, 5);
	    out_row->updated_unix_ms = sqlite3_column_int64(st, 6);
	    const unsigned char* stxt = sqlite3_column_text(st, 7);
	    out_row->status = stxt ? (const char*)stxt : "";
	    out_row->cancel_requested = sqlite3_column_int(st, 8) != 0;
	    const unsigned char* etxt = sqlite3_column_text(st, 9);
	    out_row->error = etxt ? (const char*)etxt : "";
	    const unsigned char* srtxt = sqlite3_column_text(st, 10);
	    out_row->stop_reason = srtxt ? (const char*)srtxt : "";
	    const unsigned char* rj = sqlite3_column_text(st, 11);
	    out_row->result_json = rj ? (const char*)rj : "";
	    out_row->last_heartbeat_unix_ms = sqlite3_column_int64(st, 12);
	  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::delete_job(const std::string& job_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "delete_job: job_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM jobs WHERE job_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, job_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::mark_inflight_jobs_interrupted(int64_t now_unix_ms, const std::string& reason, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  (void)reason;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const std::string r = reason.empty() ? "daemon_restart" : reason;
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE jobs
SET
  updated_unix_ms=?,
  status='interrupted',
  error=CASE WHEN error IS NULL OR error='' THEN 'interrupted by restart' ELSE error END,
  stop_reason=?
WHERE status='queued' OR status='running';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now());
  ok = ok && agent_db_bind_text(st, 2, r);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_jobs_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<JobRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT job_id, session_id, trace_id, request_json, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
FROM jobs
WHERE status=?
ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    JobRow row;
    const unsigned char* jid = sqlite3_column_text(st, 0);
    const unsigned char* sid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    const unsigned char* rq = sqlite3_column_text(st, 3);
    row.job_id = jid ? (const char*)jid : "";
    row.session_id = sid ? (const char*)sid : "";
    row.trace_id = tid ? (const char*)tid : "";
    row.request_json = rq ? (const char*)rq : "";
    row.priority = sqlite3_column_int(st, 4);
    row.created_unix_ms = sqlite3_column_int64(st, 5);
    row.updated_unix_ms = sqlite3_column_int64(st, 6);
    const unsigned char* stxt = sqlite3_column_text(st, 7);
    row.status = stxt ? (const char*)stxt : "";
    row.cancel_requested = sqlite3_column_int(st, 8) != 0;
    const unsigned char* etxt = sqlite3_column_text(st, 9);
    row.error = etxt ? (const char*)etxt : "";
    const unsigned char* srtxt = sqlite3_column_text(st, 10);
    row.stop_reason = srtxt ? (const char*)srtxt : "";
    const unsigned char* rj = sqlite3_column_text(st, 11);
    row.result_json = rj ? (const char*)rj : "";
    row.last_heartbeat_unix_ms = sqlite3_column_int64(st, 12);
    out_rows_desc->push_back(std::move(row));
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::claim_job(const std::string& job_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "claim_job: job_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE jobs
SET status='running',
    updated_unix_ms=?,
    last_heartbeat_unix_ms=?
WHERE job_id=? AND status='queued' AND (cancel_requested IS NULL OR cancel_requested=0);
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, now);
  ok = ok && agent_db_bind_text(st, 3, job_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  const int changed = sqlite3_changes(db_);
  return changed > 0;
#endif
}

bool AgentDb::recover_inflight_jobs_resumable(int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  bool ok = true;
  ok = ok && exec_locked(
    "UPDATE jobs SET status='queued', updated_unix_ms=" + std::to_string((long long)now) + " WHERE status='running';",
    out_error);
  ok = ok && exec_locked(
    "UPDATE jobs SET status='interrupted', updated_unix_ms=" + std::to_string((long long)now) +
      ", error=CASE WHEN error IS NULL OR error='' THEN 'missing request_json; cannot resume' ELSE error END"
      ", stop_reason=CASE WHEN stop_reason IS NULL OR stop_reason='' THEN 'missing_request_json' ELSE stop_reason END"
      " WHERE (status='queued' OR status='running') AND (request_json IS NULL OR request_json='');",
    out_error);
  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::recover_inflight_workflows(int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();

  // At-least-once semantics: if a task was running when the daemon died, we cannot know if it completed.
  // Recover it back to queued so it can resume.
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  bool ok = true;
  ok = ok && exec_locked("UPDATE workflows SET status='queued', updated_unix_ms=" + std::to_string((long long)now) +
                           " WHERE status='running';", out_error);
  ok = ok && exec_locked("UPDATE workflow_tasks SET status='queued', updated_unix_ms=" + std::to_string((long long)now) +
                           " WHERE status='running';", out_error);
  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::create_workflow(const WorkflowRow& wf, const std::vector<WorkflowTaskRow>& tasks, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  (void)tasks;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty()) {
    if (out_error) *out_error = "create_workflow: workflow_id is empty";
    return false;
  }
  if (wf.spec_json.empty()) {
    if (out_error) *out_error = "create_workflow: spec_json is empty";
    return false;
  }
  if (tasks.empty()) {
    if (out_error) *out_error = "create_workflow: tasks empty";
    return false;
  }

  const int64_t now = wf.updated_unix_ms > 0 ? wf.updated_unix_ms : agent_db_unix_ms_now();
  const int64_t created = wf.created_unix_ms > 0 ? wf.created_unix_ms : now;
  const std::string status = wf.status.empty() ? "queued" : wf.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;

  bool ok = true;
	  {
	    sqlite3_stmt* st = nullptr;
	    const char* sql = R"SQL(
	INSERT INTO workflows(
	  workflow_id, session_id, trace_id, priority, deadline_unix_ms, idempotency_key, created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);
	)SQL";
	    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
	      ok = false;
	      if (out_error) *out_error = agent_db_sqlite_err(db_);
	    } else {
	      ok = ok && agent_db_bind_text(st, 1, wf.workflow_id);
	      ok = ok && agent_db_bind_text_or_null(st, 2, wf.session_id);
	      ok = ok && agent_db_bind_text(st, 3, wf.trace_id);
	      ok = ok && agent_db_bind_i32_or_null(st, 4, wf.priority, AgentDb::kIntUnset);
	      ok = ok && agent_db_bind_i64_or_null(st, 5, wf.deadline_unix_ms);
	      ok = ok && agent_db_bind_text_or_null(st, 6, wf.idempotency_key);
	      ok = ok && agent_db_bind_i64(st, 7, created);
	      ok = ok && agent_db_bind_i64(st, 8, now);
	      ok = ok && agent_db_bind_text(st, 9, status);
	      ok = ok && agent_db_bind_i32(st, 10, wf.cancel_requested ? 1 : 0);
	      ok = ok && agent_db_bind_text(st, 11, wf.error);
	      ok = ok && agent_db_bind_text(st, 12, wf.spec_json);
	      ok = ok && agent_db_bind_text(st, 13, wf.result_json);
	      ok = ok && agent_db_step_done(st);
	      if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
	      sqlite3_finalize(st);
	    }
	  }

		  if (ok) {
		    sqlite3_stmt* st = nullptr;
		    const char* sql = R"SQL(
		INSERT INTO workflow_tasks(
		  workflow_id, task_id, priority, created_unix_ms, updated_unix_ms, status, allow_error, attempt, max_attempts, ready_unix_ms,
		  started_unix_ms, finished_unix_ms, depends_on_json, request_json, expect_json, result_json, error,
		  tool_calls_total_cum, steps_executed_cum, elapsed_ms_cum,
		  prompt_tokens_cum, completion_tokens_cum, total_tokens_cum
		) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
		)SQL";
	    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
	      ok = false;
	      if (out_error) *out_error = agent_db_sqlite_err(db_);
	    } else {
	      for (const auto& t : tasks) {
	        if (t.task_id.empty() || t.request_json.empty()) {
	          ok = false;
	          if (out_error && out_error->empty()) *out_error = "create_workflow: task missing task_id/request_json";
	          break;
	        }
        const int64_t t_created = t.created_unix_ms > 0 ? t.created_unix_ms : created;
        const int64_t t_updated = t.updated_unix_ms > 0 ? t.updated_unix_ms : now;
        const std::string t_status = t.status.empty() ? "queued" : t.status;
        const int t_attempt = t.attempt < 0 ? 0 : t.attempt;
        const int t_max_attempts = t.max_attempts <= 0 ? 1 : t.max_attempts;
        const int64_t t_ready = t.ready_unix_ms > 0 ? t.ready_unix_ms : 0;
        const int64_t t_started = t.started_unix_ms > 0 ? t.started_unix_ms : 0;
        const int64_t t_finished = t.finished_unix_ms > 0 ? t.finished_unix_ms : 0;

	        sqlite3_reset(st);
	        sqlite3_clear_bindings(st);
	        ok = ok && agent_db_bind_text(st, 1, wf.workflow_id);
	        ok = ok && agent_db_bind_text(st, 2, t.task_id);
	        ok = ok && agent_db_bind_i32_or_null(st, 3, t.priority, AgentDb::kIntUnset);
		        ok = ok && agent_db_bind_i64(st, 4, t_created);
		        ok = ok && agent_db_bind_i64(st, 5, t_updated);
		        ok = ok && agent_db_bind_text(st, 6, t_status);
		        ok = ok && agent_db_bind_i32(st, 7, t.allow_error ? 1 : 0);
		        ok = ok && agent_db_bind_i32(st, 8, t_attempt);
		        ok = ok && agent_db_bind_i32(st, 9, t_max_attempts);
		        ok = ok && agent_db_bind_i64(st, 10, t_ready);
		        ok = ok && agent_db_bind_i64(st, 11, t_started);
		        ok = ok && agent_db_bind_i64(st, 12, t_finished);
		        ok = ok && agent_db_bind_text(st, 13, t.depends_on_json);
		        ok = ok && agent_db_bind_text(st, 14, t.request_json);
			        ok = ok && agent_db_bind_text(st, 15, t.expect_json);
			        ok = ok && agent_db_bind_text(st, 16, t.result_json);
			        ok = ok && agent_db_bind_text(st, 17, t.error);
			        ok = ok && agent_db_bind_i64(st, 18, std::max<int64_t>(0, t.tool_calls_total_cum));
			        ok = ok && agent_db_bind_i64(st, 19, std::max<int64_t>(0, t.steps_executed_cum));
			        ok = ok && agent_db_bind_i64(st, 20, std::max<int64_t>(0, t.elapsed_ms_cum));
			        ok = ok && agent_db_bind_i64(st, 21, std::max<int64_t>(0, t.prompt_tokens_cum));
			        ok = ok && agent_db_bind_i64(st, 22, std::max<int64_t>(0, t.completion_tokens_cum));
			        ok = ok && agent_db_bind_i64(st, 23, std::max<int64_t>(0, t.total_tokens_cum));
			        ok = ok && agent_db_step_done(st);
			        if (!ok) {
			          if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
			          break;
			        }
	      }
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::get_workflow(const std::string& workflow_id, WorkflowRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = WorkflowRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "get_workflow: workflow_id is empty";
    return false;
  }
  if (!out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	WHERE workflow_id=?
	LIMIT 1;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, workflow_id);
  bool found = false;
	  if (ok && agent_db_step_row(st)) {
	    found = true;
	    const unsigned char* wid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* idk = sqlite3_column_text(st, 5);
	    const unsigned char* stxt = sqlite3_column_text(st, 8);
	    const unsigned char* etxt = sqlite3_column_text(st, 10);
	    const unsigned char* spec = sqlite3_column_text(st, 11);
	    const unsigned char* res = sqlite3_column_text(st, 12);
	    out_row->workflow_id = wid ? (const char*)wid : "";
	    out_row->session_id = sid ? (const char*)sid : "";
	    out_row->trace_id = tid ? (const char*)tid : "";
	    out_row->priority = sqlite3_column_int(st, 3);
	    out_row->deadline_unix_ms = sqlite3_column_int64(st, 4);
	    out_row->idempotency_key = idk ? (const char*)idk : "";
	    out_row->created_unix_ms = sqlite3_column_int64(st, 6);
	    out_row->updated_unix_ms = sqlite3_column_int64(st, 7);
	    out_row->status = stxt ? (const char*)stxt : "";
	    out_row->cancel_requested = sqlite3_column_int(st, 9) != 0;
	    out_row->error = etxt ? (const char*)etxt : "";
	    out_row->spec_json = spec ? (const char*)spec : "";
	    out_row->result_json = res ? (const char*)res : "";
	  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::get_workflow_by_idempotency_key(
  const std::string& session_id_or_empty,
  const std::string& idempotency_key,
  WorkflowRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = WorkflowRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id_or_empty;
  (void)idempotency_key;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (idempotency_key.empty()) {
    if (out_error) *out_error = "get_workflow_by_idempotency_key: idempotency_key is empty";
    return false;
  }
  if (!out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
FROM workflows
WHERE idempotency_key=? AND COALESCE(session_id,'')=?
ORDER BY created_unix_ms DESC
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, idempotency_key);
  ok = ok && agent_db_bind_text(st, 2, session_id_or_empty);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* sid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    const unsigned char* idk = sqlite3_column_text(st, 5);
    const unsigned char* stxt = sqlite3_column_text(st, 8);
    const unsigned char* etxt = sqlite3_column_text(st, 10);
    const unsigned char* spec = sqlite3_column_text(st, 11);
    const unsigned char* res = sqlite3_column_text(st, 12);
    out_row->workflow_id = wid ? (const char*)wid : "";
    out_row->session_id = sid ? (const char*)sid : "";
    out_row->trace_id = tid ? (const char*)tid : "";
    out_row->priority = sqlite3_column_int(st, 3);
    out_row->deadline_unix_ms = sqlite3_column_int64(st, 4);
    out_row->idempotency_key = idk ? (const char*)idk : "";
    out_row->created_unix_ms = sqlite3_column_int64(st, 6);
    out_row->updated_unix_ms = sqlite3_column_int64(st, 7);
    out_row->status = stxt ? (const char*)stxt : "";
    out_row->cancel_requested = sqlite3_column_int(st, 9) != 0;
    out_row->error = etxt ? (const char*)etxt : "";
    out_row->spec_json = spec ? (const char*)spec : "";
    out_row->result_json = res ? (const char*)res : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_workflows_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<WorkflowRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const bool list_all = status == "all" || status == "*";
  const bool list_active = status == "active";
	sqlite3_stmt* st = nullptr;
	const char* sql = list_all ? R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
	LIMIT ?;
	)SQL" : list_active ? R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	WHERE status IN ('running','queued')
	ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
	LIMIT ?;
	)SQL" : R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	WHERE status=?
	ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
	LIMIT ?;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  if (list_all || list_active) {
    ok = ok && agent_db_bind_i64(st, 1, (int64_t)max_rows);
  } else {
    ok = ok && agent_db_bind_text(st, 1, status);
    ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  }
	  while (ok && agent_db_step_row(st)) {
	    WorkflowRow row;
	    const unsigned char* wid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* idk = sqlite3_column_text(st, 5);
	    const unsigned char* stxt = sqlite3_column_text(st, 8);
	    const unsigned char* etxt = sqlite3_column_text(st, 10);
	    const unsigned char* spec = sqlite3_column_text(st, 11);
	    const unsigned char* res = sqlite3_column_text(st, 12);
	    row.workflow_id = wid ? (const char*)wid : "";
	    row.session_id = sid ? (const char*)sid : "";
	    row.trace_id = tid ? (const char*)tid : "";
	    row.priority = sqlite3_column_int(st, 3);
	    row.deadline_unix_ms = sqlite3_column_int64(st, 4);
	    row.idempotency_key = idk ? (const char*)idk : "";
	    row.created_unix_ms = sqlite3_column_int64(st, 6);
	    row.updated_unix_ms = sqlite3_column_int64(st, 7);
	    row.status = stxt ? (const char*)stxt : "";
	    row.cancel_requested = sqlite3_column_int(st, 9) != 0;
	    row.error = etxt ? (const char*)etxt : "";
	    row.spec_json = spec ? (const char*)spec : "";
	    row.result_json = res ? (const char*)res : "";
	    out_rows_desc->push_back(std::move(row));
	  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_workflows_by_status_for_scheduler(
  const std::string& status,
  size_t max_rows,
  std::vector<WorkflowRow>* out_rows,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), COALESCE(deadline_unix_ms,0), COALESCE(idempotency_key,''), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
FROM workflows
WHERE status=?
ORDER BY COALESCE(priority,0) DESC, created_unix_ms ASC, workflow_id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    WorkflowRow row;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* sid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    const unsigned char* idk = sqlite3_column_text(st, 5);
    const unsigned char* stxt = sqlite3_column_text(st, 8);
    const unsigned char* etxt = sqlite3_column_text(st, 10);
    const unsigned char* spec = sqlite3_column_text(st, 11);
    const unsigned char* res = sqlite3_column_text(st, 12);
    row.workflow_id = wid ? (const char*)wid : "";
    row.session_id = sid ? (const char*)sid : "";
    row.trace_id = tid ? (const char*)tid : "";
    row.priority = sqlite3_column_int(st, 3);
    row.deadline_unix_ms = sqlite3_column_int64(st, 4);
    row.idempotency_key = idk ? (const char*)idk : "";
    row.created_unix_ms = sqlite3_column_int64(st, 6);
    row.updated_unix_ms = sqlite3_column_int64(st, 7);
    row.status = stxt ? (const char*)stxt : "";
    row.cancel_requested = sqlite3_column_int(st, 9) != 0;
    row.error = etxt ? (const char*)etxt : "";
    row.spec_json = spec ? (const char*)spec : "";
    row.result_json = res ? (const char*)res : "";
    out_rows->push_back(std::move(row));
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_workflow_tasks(const std::string& workflow_id, std::vector<WorkflowTaskRow>* out_rows, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_workflow_tasks: workflow_id is empty";
    return false;
  }
  if (!out_rows) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
		sqlite3_stmt* st = nullptr;
		const char* sql = R"SQL(
		SELECT task_id, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, attempt, max_attempts, ready_unix_ms, started_unix_ms, finished_unix_ms,
		       depends_on_json, request_json, expect_json, result_json, error, COALESCE(allow_error,0),
		       COALESCE(tool_calls_total_cum,0), COALESCE(steps_executed_cum,0), COALESCE(elapsed_ms_cum,0),
		       COALESCE(prompt_tokens_cum,0), COALESCE(completion_tokens_cum,0), COALESCE(total_tokens_cum,0)
		FROM workflow_tasks
		WHERE workflow_id=?
		ORDER BY created_unix_ms ASC, task_id ASC;
		)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, workflow_id);
	  while (ok && agent_db_step_row(st)) {
	    WorkflowTaskRow row;
	    row.workflow_id = workflow_id;
	    const unsigned char* tid = sqlite3_column_text(st, 0);
	    row.task_id = tid ? (const char*)tid : "";
	    row.priority = sqlite3_column_int(st, 1);
	    row.created_unix_ms = sqlite3_column_int64(st, 2);
	    row.updated_unix_ms = sqlite3_column_int64(st, 3);
	    const unsigned char* stxt = sqlite3_column_text(st, 4);
	    row.status = stxt ? (const char*)stxt : "";
	    row.attempt = sqlite3_column_int(st, 5);
	    row.max_attempts = sqlite3_column_int(st, 6);
	    row.ready_unix_ms = sqlite3_column_int64(st, 7);
	    row.started_unix_ms = sqlite3_column_int64(st, 8);
	    row.finished_unix_ms = sqlite3_column_int64(st, 9);
	    const unsigned char* deps = sqlite3_column_text(st, 10);
	    const unsigned char* req = sqlite3_column_text(st, 11);
	    const unsigned char* exp = sqlite3_column_text(st, 12);
		    const unsigned char* res = sqlite3_column_text(st, 13);
		    const unsigned char* etxt = sqlite3_column_text(st, 14);
		    row.allow_error = sqlite3_column_int(st, 15) != 0;
		    row.tool_calls_total_cum = sqlite3_column_int64(st, 16);
		    row.steps_executed_cum = sqlite3_column_int64(st, 17);
		    row.elapsed_ms_cum = sqlite3_column_int64(st, 18);
		    row.prompt_tokens_cum = sqlite3_column_int64(st, 19);
		    row.completion_tokens_cum = sqlite3_column_int64(st, 20);
		    row.total_tokens_cum = sqlite3_column_int64(st, 21);
		    row.depends_on_json = deps ? (const char*)deps : "";
		    row.request_json = req ? (const char*)req : "";
		    row.expect_json = exp ? (const char*)exp : "";
		    row.result_json = res ? (const char*)res : "";
		    row.error = etxt ? (const char*)etxt : "";
		    out_rows->push_back(std::move(row));
		  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::count_workflow_inflight_tasks_total(int64_t* out_count, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_count) *out_count = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_count) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks
WHERE status IN ('queued','running');
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  if (ok && sqlite3_step(st) == SQLITE_ROW) {
    *out_count = sqlite3_column_int64(st, 0);
  } else {
    ok = false;
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::count_workflow_inflight_tasks_for_session(
  const std::string& session_id_or_empty,
  int64_t* out_count,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_count) *out_count = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id_or_empty;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_count) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks t
JOIN workflows w ON w.workflow_id=t.workflow_id
WHERE COALESCE(w.session_id,'')=? AND t.status IN ('queued','running');
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, session_id_or_empty);
  if (ok && sqlite3_step(st) == SQLITE_ROW) {
    *out_count = sqlite3_column_int64(st, 0);
  } else {
    ok = false;
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_workflow_scheduler_stats(
  int64_t now_unix_ms,
  WorkflowSchedulerStats* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = WorkflowSchedulerStats{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_stats) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const int64_t now = (now_unix_ms > 0) ? now_unix_ms : agent_db_unix_ms_now();
  out_stats->now_unix_ms = now;

  auto read_group_counts = [&](const char* sql, std::map<std::string, int64_t>* out) -> bool {
    if (!out) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      return false;
    }
    bool ok = true;
    while (ok && agent_db_step_row(st)) {
      const unsigned char* s = sqlite3_column_text(st, 0);
      const std::string key = s ? (const char*)s : "";
      const int64_t cnt = sqlite3_column_int64(st, 1);
      if (!key.empty()) (*out)[key] = cnt;
    }
    if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return ok;
  };

  if (!read_group_counts("SELECT status, COUNT(1) FROM workflows GROUP BY status;", &out_stats->workflows_by_status)) {
    return false;
  }
  if (!read_group_counts("SELECT status, COUNT(1) FROM workflow_tasks GROUP BY status;", &out_stats->tasks_by_status)) {
    return false;
  }

  // queued ready / not-ready split (useful for backpressure/debugging).
  {
    sqlite3_stmt* st = nullptr;
    const char* sql_ready = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks
WHERE status='queued' AND (ready_unix_ms IS NULL OR ready_unix_ms<=?);
)SQL";
    if (sqlite3_prepare_v2(db_, sql_ready, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      return false;
    }
    bool ok = true;
    ok = ok && agent_db_bind_i64(st, 1, now);
    if (ok && sqlite3_step(st) == SQLITE_ROW) {
      out_stats->tasks_queued_ready = sqlite3_column_int64(st, 0);
    } else if (!ok) {
      if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    sqlite3_finalize(st);
  }
  {
    sqlite3_stmt* st = nullptr;
    const char* sql_not_ready = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks
WHERE status='queued' AND ready_unix_ms IS NOT NULL AND ready_unix_ms>?;
)SQL";
    if (sqlite3_prepare_v2(db_, sql_not_ready, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      return false;
    }
    bool ok = true;
    ok = ok && agent_db_bind_i64(st, 1, now);
    if (ok && sqlite3_step(st) == SQLITE_ROW) {
      out_stats->tasks_queued_not_ready = sqlite3_column_int64(st, 0);
    } else if (!ok) {
      if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    sqlite3_finalize(st);
  }

  return true;
#endif
}

bool AgentDb::upsert_workflow(const WorkflowRow& wf, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty()) {
    if (out_error) *out_error = "upsert_workflow: workflow_id is empty";
    return false;
  }
  const int64_t now = wf.updated_unix_ms > 0 ? wf.updated_unix_ms : agent_db_unix_ms_now();
  const int64_t created = wf.created_unix_ms > 0 ? wf.created_unix_ms : now;
  const std::string status = wf.status.empty() ? "queued" : wf.status;
  const std::string spec = wf.spec_json.empty() ? "{}" : wf.spec_json;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	INSERT INTO workflows(
	  workflow_id, session_id, trace_id, priority, deadline_unix_ms, idempotency_key, created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
	ON CONFLICT(workflow_id) DO UPDATE SET
	  session_id = CASE WHEN excluded.session_id IS NOT NULL AND excluded.session_id <> '' THEN excluded.session_id ELSE workflows.session_id END,
	  trace_id = CASE WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id ELSE workflows.trace_id END,
	  priority = CASE WHEN excluded.priority IS NOT NULL THEN excluded.priority ELSE workflows.priority END,
	  deadline_unix_ms = CASE
	    WHEN excluded.deadline_unix_ms IS NOT NULL AND excluded.deadline_unix_ms > 0 THEN excluded.deadline_unix_ms
	    ELSE workflows.deadline_unix_ms
	  END,
	  idempotency_key = CASE
	    WHEN excluded.idempotency_key IS NOT NULL AND excluded.idempotency_key <> '' THEN excluded.idempotency_key
	    ELSE workflows.idempotency_key
	  END,
	  updated_unix_ms = excluded.updated_unix_ms,
	  status = excluded.status,
	  cancel_requested = excluded.cancel_requested,
	  error = excluded.error,
	  spec_json = excluded.spec_json,
	  result_json = excluded.result_json;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
	bool ok = true;
	ok = ok && agent_db_bind_text(st, 1, wf.workflow_id);
	ok = ok && agent_db_bind_text_or_null(st, 2, wf.session_id);
	ok = ok && agent_db_bind_text(st, 3, wf.trace_id);
	ok = ok && agent_db_bind_i32_or_null(st, 4, wf.priority, AgentDb::kIntUnset);
	ok = ok && agent_db_bind_i64_or_null(st, 5, wf.deadline_unix_ms);
	ok = ok && agent_db_bind_text_or_null(st, 6, wf.idempotency_key);
	ok = ok && agent_db_bind_i64(st, 7, created);
	ok = ok && agent_db_bind_i64(st, 8, now);
	ok = ok && agent_db_bind_text(st, 9, status);
	ok = ok && agent_db_bind_i32(st, 10, wf.cancel_requested ? 1 : 0);
	ok = ok && agent_db_bind_text(st, 11, wf.error);
	ok = ok && agent_db_bind_text(st, 12, spec);
	ok = ok && agent_db_bind_text(st, 13, wf.result_json);
	ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::upsert_workflow_task(const WorkflowTaskRow& task, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (task.workflow_id.empty() || task.task_id.empty()) {
    if (out_error) *out_error = "upsert_workflow_task: workflow_id/task_id empty";
    return false;
  }
  const int64_t now = task.updated_unix_ms > 0 ? task.updated_unix_ms : agent_db_unix_ms_now();
  const int64_t created = task.created_unix_ms > 0 ? task.created_unix_ms : now;
  const std::string status = task.status.empty() ? "queued" : task.status;
  const int attempt = task.attempt < 0 ? 0 : task.attempt;
  const int max_attempts = task.max_attempts <= 0 ? 1 : task.max_attempts;
  const int64_t ready = task.ready_unix_ms > 0 ? task.ready_unix_ms : 0;
  const int64_t started = task.started_unix_ms > 0 ? task.started_unix_ms : 0;
  const int64_t finished = task.finished_unix_ms > 0 ? task.finished_unix_ms : 0;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	  sqlite3_stmt* st = nullptr;
		const char* sql = R"SQL(
		INSERT INTO workflow_tasks(
		  workflow_id, task_id, priority, created_unix_ms, updated_unix_ms, status, allow_error, attempt, max_attempts, ready_unix_ms,
		  started_unix_ms, finished_unix_ms, depends_on_json, request_json, expect_json, result_json, error,
		  tool_calls_total_cum, steps_executed_cum, elapsed_ms_cum,
		  prompt_tokens_cum, completion_tokens_cum, total_tokens_cum
		) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(workflow_id, task_id) DO UPDATE SET
		  priority = CASE WHEN excluded.priority IS NOT NULL THEN excluded.priority ELSE workflow_tasks.priority END,
		  updated_unix_ms = excluded.updated_unix_ms,
		  status = excluded.status,
		  allow_error = excluded.allow_error,
		  attempt = excluded.attempt,
		  max_attempts = excluded.max_attempts,
		  ready_unix_ms = excluded.ready_unix_ms,
		  started_unix_ms = excluded.started_unix_ms,
		  finished_unix_ms = excluded.finished_unix_ms,
		  depends_on_json = excluded.depends_on_json,
		  request_json = excluded.request_json,
		  expect_json = excluded.expect_json,
		  result_json = excluded.result_json,
		  error = excluded.error,
		  tool_calls_total_cum = CASE
		    WHEN excluded.tool_calls_total_cum >= COALESCE(workflow_tasks.tool_calls_total_cum,0) THEN excluded.tool_calls_total_cum
		    ELSE COALESCE(workflow_tasks.tool_calls_total_cum,0)
		  END,
		  steps_executed_cum = CASE
		    WHEN excluded.steps_executed_cum >= COALESCE(workflow_tasks.steps_executed_cum,0) THEN excluded.steps_executed_cum
		    ELSE COALESCE(workflow_tasks.steps_executed_cum,0)
		  END,
		  elapsed_ms_cum = CASE
		    WHEN excluded.elapsed_ms_cum >= COALESCE(workflow_tasks.elapsed_ms_cum,0) THEN excluded.elapsed_ms_cum
		    ELSE COALESCE(workflow_tasks.elapsed_ms_cum,0)
		  END,
		  prompt_tokens_cum = CASE
		    WHEN excluded.prompt_tokens_cum >= COALESCE(workflow_tasks.prompt_tokens_cum,0) THEN excluded.prompt_tokens_cum
		    ELSE COALESCE(workflow_tasks.prompt_tokens_cum,0)
		  END,
		  completion_tokens_cum = CASE
		    WHEN excluded.completion_tokens_cum >= COALESCE(workflow_tasks.completion_tokens_cum,0) THEN excluded.completion_tokens_cum
		    ELSE COALESCE(workflow_tasks.completion_tokens_cum,0)
		  END,
		  total_tokens_cum = CASE
		    WHEN excluded.total_tokens_cum >= COALESCE(workflow_tasks.total_tokens_cum,0) THEN excluded.total_tokens_cum
		    ELSE COALESCE(workflow_tasks.total_tokens_cum,0)
		  END;
		)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
	bool ok = true;
	ok = ok && agent_db_bind_text(st, 1, task.workflow_id);
	ok = ok && agent_db_bind_text(st, 2, task.task_id);
	ok = ok && agent_db_bind_i32_or_null(st, 3, task.priority, AgentDb::kIntUnset);
	ok = ok && agent_db_bind_i64(st, 4, created);
	ok = ok && agent_db_bind_i64(st, 5, now);
	ok = ok && agent_db_bind_text(st, 6, status);
	ok = ok && agent_db_bind_i32(st, 7, task.allow_error ? 1 : 0);
	ok = ok && agent_db_bind_i32(st, 8, attempt);
	ok = ok && agent_db_bind_i32(st, 9, max_attempts);
	ok = ok && agent_db_bind_i64(st, 10, ready);
	ok = ok && agent_db_bind_i64(st, 11, started);
	ok = ok && agent_db_bind_i64(st, 12, finished);
	ok = ok && agent_db_bind_text(st, 13, task.depends_on_json);
	ok = ok && agent_db_bind_text(st, 14, task.request_json);
		ok = ok && agent_db_bind_text(st, 15, task.expect_json);
		ok = ok && agent_db_bind_text(st, 16, task.result_json);
		ok = ok && agent_db_bind_text(st, 17, task.error);
		ok = ok && agent_db_bind_i64(st, 18, std::max<int64_t>(0, task.tool_calls_total_cum));
		ok = ok && agent_db_bind_i64(st, 19, std::max<int64_t>(0, task.steps_executed_cum));
		ok = ok && agent_db_bind_i64(st, 20, std::max<int64_t>(0, task.elapsed_ms_cum));
		ok = ok && agent_db_bind_i64(st, 21, std::max<int64_t>(0, task.prompt_tokens_cum));
		ok = ok && agent_db_bind_i64(st, 22, std::max<int64_t>(0, task.completion_tokens_cum));
		ok = ok && agent_db_bind_i64(st, 23, std::max<int64_t>(0, task.total_tokens_cum));
		ok = ok && agent_db_step_done(st);
	  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
	  sqlite3_finalize(st);
	  return ok;
#endif
}

bool AgentDb::cancel_workflow_task_if_queued(
  const std::string& workflow_id,
  const std::string& task_id,
  int64_t now_unix_ms,
  const std::string& error,
  const std::string& result_json,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)task_id;
  (void)now_unix_ms;
  (void)error;
  (void)result_json;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || task_id.empty()) {
    if (out_error) *out_error = "cancel_workflow_task_if_queued: workflow_id/task_id empty";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_tasks
SET
  status='cancelled',
  updated_unix_ms=?,
  finished_unix_ms=?,
  ready_unix_ms=0,
  error=?,
  result_json=?
WHERE workflow_id=? AND task_id=? AND status='queued';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, now);
  ok = ok && agent_db_bind_text_or_null(st, 3, error);
  ok = ok && agent_db_bind_text_or_null(st, 4, result_json);
  ok = ok && agent_db_bind_text(st, 5, workflow_id);
  ok = ok && agent_db_bind_text(st, 6, task_id);
  ok = ok && agent_db_step_done(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed > 0;
#endif
}

bool AgentDb::claim_workflow_task(
  const std::string& workflow_id,
  const std::string& task_id,
  int64_t now_unix_ms,
  int new_attempt,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)task_id;
  (void)now_unix_ms;
  (void)new_attempt;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || task_id.empty()) {
    if (out_error) *out_error = "claim_workflow_task: workflow_id/task_id empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_tasks
SET
  status='running',
  updated_unix_ms=?,
  started_unix_ms=CASE WHEN started_unix_ms IS NULL OR started_unix_ms=0 THEN ? ELSE started_unix_ms END,
  attempt=?
WHERE workflow_id=? AND task_id=? AND status='queued';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, now);
  ok = ok && agent_db_bind_i32(st, 3, new_attempt < 0 ? 0 : new_attempt);
  ok = ok && agent_db_bind_text(st, 4, workflow_id);
  ok = ok && agent_db_bind_text(st, 5, task_id);
  ok = ok && agent_db_step_done(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed > 0;
#endif
}

bool AgentDb::claim_workflow_task_budgeted(
  const std::string& workflow_id,
  const std::string& task_id,
  int64_t now_unix_ms,
  int new_attempt,
  int max_inflight_per_workflow,
  int max_inflight_per_session,
  const std::string& session_id,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)task_id;
  (void)now_unix_ms;
  (void)new_attempt;
  (void)max_inflight_per_workflow;
  (void)max_inflight_per_session;
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || task_id.empty()) {
    if (out_error) *out_error = "claim_workflow_task_budgeted: workflow_id/task_id empty";
    return false;
  }
  if (max_inflight_per_workflow <= 0 && max_inflight_per_session <= 0) {
    return claim_workflow_task(workflow_id, task_id, now_unix_ms, new_attempt, out_error);
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  auto count_i64 = [&](const char* sql, std::function<bool(sqlite3_stmt*)> binder, int64_t* out) -> bool {
    if (!out) return false;
    *out = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
      return false;
    }
    bool ok = true;
    if (binder) ok = ok && binder(st);
    if (ok) {
      const int rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
        ok = true;
      } else if (rc == SQLITE_DONE) {
        *out = 0;
        ok = true;
      } else {
        ok = false;
      }
    }
    if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return ok;
  };

  if (max_inflight_per_workflow > 0) {
    int64_t running = 0;
    const char* sql = "SELECT COUNT(1) FROM workflow_tasks WHERE workflow_id=? AND status='running';";
    if (!count_i64(sql, [&](sqlite3_stmt* st) { return agent_db_bind_text(st, 1, workflow_id); }, &running)) {
      return false;
    }
    if (running >= (int64_t)max_inflight_per_workflow) {
      // Budget exceeded: not an error; just don't claim.
      return false;
    }
  }

  if (max_inflight_per_session > 0 && !session_id.empty()) {
    int64_t running = 0;
    const char* sql = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks t
JOIN workflows w ON w.workflow_id = t.workflow_id
WHERE w.session_id=? AND t.status='running';
)SQL";
    if (!count_i64(sql, [&](sqlite3_stmt* st) { return agent_db_bind_text(st, 1, session_id); }, &running)) {
      return false;
    }
    if (running >= (int64_t)max_inflight_per_session) {
      // Budget exceeded: not an error; just don't claim.
      return false;
    }
  }

  // Claim the task (same semantics as claim_workflow_task).
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_tasks
SET
  status='running',
  updated_unix_ms=?,
  started_unix_ms=CASE WHEN started_unix_ms IS NULL OR started_unix_ms=0 THEN ? ELSE started_unix_ms END,
  attempt=?
WHERE workflow_id=? AND task_id=? AND status='queued';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, now);
  ok = ok && agent_db_bind_i32(st, 3, new_attempt < 0 ? 0 : new_attempt);
  ok = ok && agent_db_bind_text(st, 4, workflow_id);
  ok = ok && agent_db_bind_text(st, 5, task_id);
  ok = ok && agent_db_step_done(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed > 0;
#endif
}


bool AgentDb::insert_workflow_event(const WorkflowEventRow& row, int64_t* out_event_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_event_id) *out_event_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.workflow_id.empty()) {
    if (out_error) *out_error = "insert_workflow_event: workflow_id is empty";
    return false;
  }
  if (row.type.empty()) {
    if (out_error) *out_error = "insert_workflow_event: type is empty";
    return false;
  }
  if (row.data_json.empty()) {
    if (out_error) *out_error = "insert_workflow_event: data_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const char* sql = "INSERT INTO workflow_events(workflow_id, task_id, ts_unix_ms, type, data_json) VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  const int64_t ts = row.ts_unix_ms > 0 ? row.ts_unix_ms : agent_db_unix_ms_now();
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.workflow_id);
  ok = ok && agent_db_bind_text_or_null(st, 2, row.task_id);
  ok = ok && agent_db_bind_i64(st, 3, ts);
  ok = ok && agent_db_bind_text(st, 4, row.type);
  ok = ok && agent_db_bind_text(st, 5, row.data_json);

  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!agent_db_step_done(st)) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  const int64_t eid = (int64_t)sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  if (out_event_id) *out_event_id = eid;
  return true;
#endif
}

bool AgentDb::list_workflow_events(
  const std::string& workflow_id,
  int64_t after_event_id,
  size_t max_rows,
  std::vector<WorkflowEventRow>* out_rows_asc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_asc) out_rows_asc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)after_event_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_workflow_events: workflow_id is empty";
    return false;
  }
  if (!out_rows_asc) return false;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const size_t lim = std::min<size_t>(max_rows == 0 ? 256 : max_rows, 1000);
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT event_id, workflow_id, task_id, ts_unix_ms, type, data_json
FROM workflow_events
WHERE workflow_id=? AND event_id>?
ORDER BY event_id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, workflow_id);
  ok = ok && agent_db_bind_i64(st, 2, after_event_id < 0 ? 0 : after_event_id);
  ok = ok && agent_db_bind_i64(st, 3, (int64_t)lim);
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  while (agent_db_step_row(st)) {
    WorkflowEventRow r;
    r.event_id = sqlite3_column_int64(st, 0);
    const unsigned char* wid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    r.workflow_id = wid ? (const char*)wid : "";
    r.task_id = tid ? (const char*)tid : "";
    r.ts_unix_ms = sqlite3_column_int64(st, 3);
    const unsigned char* type = sqlite3_column_text(st, 4);
    const unsigned char* dj = sqlite3_column_text(st, 5);
    r.type = type ? (const char*)type : "";
    r.data_json = dj ? (const char*)dj : "";
    out_rows_asc->push_back(std::move(r));
    if (out_rows_asc->size() >= lim) break;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::get_workflow_usage_totals(
  const std::string& workflow_id,
  WorkflowUsageTotals* out_totals,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_totals) *out_totals = WorkflowUsageTotals{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_totals) return false;
  if (workflow_id.empty()) {
    if (out_error) *out_error = "get_workflow_usage_totals: workflow_id is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT
  SUM(COALESCE(tool_calls_total_cum,0)),
  SUM(COALESCE(steps_executed_cum,0)),
  SUM(COALESCE(elapsed_ms_cum,0)),
  SUM(COALESCE(prompt_tokens_cum,0)),
  SUM(COALESCE(completion_tokens_cum,0)),
  SUM(COALESCE(total_tokens_cum,0))
FROM workflow_tasks
WHERE workflow_id=?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, workflow_id);
  if (ok && sqlite3_step(st) == SQLITE_ROW) {
    out_totals->tool_calls_total_used = sqlite3_column_int64(st, 0);
    out_totals->steps_total_used = sqlite3_column_int64(st, 1);
    out_totals->elapsed_ms_total_used = sqlite3_column_int64(st, 2);
    out_totals->prompt_tokens_used = sqlite3_column_int64(st, 3);
    out_totals->completion_tokens_used = sqlite3_column_int64(st, 4);
    out_totals->total_tokens_used = sqlite3_column_int64(st, 5);
  } else {
    ok = false;
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_workflow_session_stats(
  size_t max_rows,
  bool include_no_session,
  std::vector<WorkflowSessionStatsRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)max_rows;
  (void)include_no_session;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) return true;
  if (max_rows > 512) max_rows = 512;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT
  COALESCE(w.session_id,'') AS sid,
  SUM(CASE WHEN t.status IN ('queued','running') THEN 1 ELSE 0 END) AS inflight_tasks,
  SUM(CASE WHEN t.status='queued' THEN 1 ELSE 0 END) AS queued_tasks,
  SUM(CASE WHEN t.status='running' THEN 1 ELSE 0 END) AS running_tasks,
  COUNT(DISTINCT CASE WHEN w.status='queued' THEN w.workflow_id ELSE NULL END) AS workflows_queued,
  COUNT(DISTINCT CASE WHEN w.status='running' THEN w.workflow_id ELSE NULL END) AS workflows_running
FROM workflow_tasks t
JOIN workflows w ON w.workflow_id=t.workflow_id
WHERE (? != 0 OR COALESCE(w.session_id,'') != '')
GROUP BY sid
ORDER BY inflight_tasks DESC, sid ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, include_no_session ? 1 : 0);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    WorkflowSessionStatsRow row;
    const unsigned char* sid = sqlite3_column_text(st, 0);
    row.session_id = sid ? (const char*)sid : "";
    row.inflight_tasks = sqlite3_column_int64(st, 1);
    row.queued_tasks = sqlite3_column_int64(st, 2);
    row.running_tasks = sqlite3_column_int64(st, 3);
    row.workflows_queued = sqlite3_column_int64(st, 4);
    row.workflows_running = sqlite3_column_int64(st, 5);
    out_rows_desc->push_back(std::move(row));
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_workflow_fairq_session(
  const std::string& session_id,
  WorkflowFairqSessionRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = WorkflowFairqSessionRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_row || session_id.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT session_id, deficit, weight, updated_unix_ms
FROM workflow_fairq_sessions
WHERE session_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* sid = sqlite3_column_text(st, 0);
    out_row->session_id = sid ? (const char*)sid : "";
    out_row->deficit = sqlite3_column_int64(st, 1);
    out_row->weight = (int)sqlite3_column_int64(st, 2);
    out_row->updated_unix_ms = sqlite3_column_int64(st, 3);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::upsert_workflow_fairq_session(const WorkflowFairqSessionRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "upsert_workflow_fairq_session: session_id is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO workflow_fairq_sessions(session_id, deficit, weight, updated_unix_ms)
VALUES(?,?,?,?)
ON CONFLICT(session_id) DO UPDATE SET
  deficit = excluded.deficit,
  weight = excluded.weight,
  updated_unix_ms = excluded.updated_unix_ms;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.session_id);
  ok = ok && agent_db_bind_i64(st, 2, row.deficit);
  ok = ok && agent_db_bind_i64(st, 3, (int64_t)row.weight);
  ok = ok && agent_db_bind_i64(st, 4, row.updated_unix_ms);
  if (ok && !agent_db_step_done(st)) ok = false;
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::delete_workflow_fairq_sessions_older_than(
  int64_t cutoff_unix_ms,
  int64_t* out_deleted,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_deleted) *out_deleted = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)cutoff_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (cutoff_unix_ms <= 0) return true;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM workflow_fairq_sessions WHERE updated_unix_ms < ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, cutoff_unix_ms);
  if (ok && !agent_db_step_done(st)) ok = false;
  const int64_t deleted = ok ? (int64_t)sqlite3_changes(db_) : 0;
  if (out_deleted) *out_deleted = deleted;
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

}  // namespace agentd
