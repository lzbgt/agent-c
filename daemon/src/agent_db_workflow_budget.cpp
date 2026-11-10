#include "agent_db.h"
#include "agent_db_sqlite.h"
#include <algorithm>
#include <limits>
#include <memory>

namespace agentd {
#if defined(AGENT_HAVE_SQLITE3)
namespace {
using Amounts = AgentDb::WorkflowBudgetAmounts;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement query(sqlite3 *db, const char *sql) {
  sqlite3_stmt *s = nullptr;
  sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
  return Statement(s, sqlite3_finalize);
}
int64_t add(int64_t a, int64_t b) {
  a = std::max<int64_t>(0, a);
  b = std::max<int64_t>(0, b);
  return a > INT64_MAX - b ? INT64_MAX : a + b;
}
void accumulate(Amounts *out, sqlite3_stmt *row, int start = 0) {
  for (int i = 0; i < 4; i++)
    (*out)[i] = add((*out)[i], sqlite3_column_int64(row, start + i));
}
bool snapshot(sqlite3 *db, const std::string &id,
              AgentDb::WorkflowBudgetSnapshot *out) {
  *out = {};
  auto tasks =
      query(db, "SELECT "
                "tool_calls_total_cum,steps_executed_cum,elapsed_ms_cum,total_"
                "tokens_cum FROM workflow_tasks WHERE workflow_id=?");
  if (!tasks || !agent_db_bind_text(tasks.get(), 1, id))
    return false;
  int code = SQLITE_DONE;
  while ((code = sqlite3_step(tasks.get())) == SQLITE_ROW)
    accumulate(&out->charged, tasks.get());
  if (code != SQLITE_DONE)
    return false;
  auto r = query(db, "SELECT state,tools,steps,elapsed_ms,tokens FROM "
                     "workflow_budget_reservations WHERE workflow_id=? AND "
                     "state IN ('reserved','lost')");
  if (!r || !agent_db_bind_text(r.get(), 1, id))
    return false;
  while ((code = sqlite3_step(r.get())) == SQLITE_ROW) {
    const auto *state = sqlite3_column_text(r.get(), 0);
    if (state &&
        std::string(reinterpret_cast<const char *>(state)) == "reserved") {
      accumulate(&out->held, r.get(), 1);
      out->active_attempts++;
    } else {
      accumulate(&out->uncertain, r.get(), 1);
      accumulate(&out->charged, r.get(), 1);
      out->uncertain_attempts++;
    }
  }
  return code == SQLITE_DONE;
}
bool bind_expected(sqlite3_stmt *s, int start,
                   const AgentDb::WorkflowTaskRow &task) {
  return agent_db_bind_text(s, start, task.workflow_id) &&
         agent_db_bind_text(s, start + 1, task.task_id) &&
         agent_db_bind_i32(s, start + 2, task.attempt) &&
         agent_db_bind_text(s, start + 3, task.request_json);
}
} // namespace
#endif

bool AgentDb::reserve_workflow_budget(const WorkflowRow &workflow,
                                      const WorkflowTaskRow &task,
                                      const WorkflowBudgetAmounts &limits,
                                      const WorkflowBudgetAmounts &demand,
                                      const WorkflowBudgetAmounts &minimum,
                                      WorkflowBudgetGrant *out,
                                      std::string *error) {
  if (error)
    error->clear();
  if (out)
    *out = {};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow;
  (void)task;
  (void)limits;
  (void)demand;
  (void)minimum;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  if (!out || workflow.workflow_id != task.workflow_id || task.attempt <= 0)
    return false;
  for (int i = 0; i < 4; i++)
    if (limits[i] < 0 || demand[i] < 0 || minimum[i] < 0 ||
        minimum[i] > demand[i]) {
      if (error)
        *error = "invalid workflow budget demand";
      return false;
    }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN IMMEDIATE", error))
    return false;
  const auto fail = [&](const std::string &reason) {
    exec_locked("ROLLBACK", nullptr);
    if (error)
      *error = reason;
    return false;
  };
  {
    auto s = query(
        db_,
        "SELECT w.cancel_requested,COALESCE(w.deadline_unix_ms,0),w.spec_json "
        "FROM workflows w JOIN workflow_tasks t ON t.workflow_id=w.workflow_id "
        "WHERE t.workflow_id=? AND t.task_id=? AND t.attempt=? AND "
        "t.request_json=? AND t.status='running'");
    if (!s || !bind_expected(s.get(), 1, task) ||
        sqlite3_step(s.get()) != SQLITE_ROW)
      return fail("stale workflow attempt cannot reserve budget");
    const auto *spec = sqlite3_column_text(s.get(), 2);
    if (!spec || workflow.spec_json != reinterpret_cast<const char *>(spec))
      return fail("workflow definition changed before reservation");
    if (sqlite3_column_int(s.get(), 0) ||
        (sqlite3_column_int64(s.get(), 1) > 0 &&
         sqlite3_column_int64(s.get(), 1) <= agent_db_unix_ms_now())) {
      out->status = "stopped";
      exec_locked("ROLLBACK", nullptr);
      return true;
    }
  }
  // Unbounded autonomous work needs attempt CAS, not a zero-value grant
  // journal.
  if (std::all_of(limits.begin(), limits.end(),
                  [](int64_t value) { return value == 0; })) {
    if (!exec_locked("COMMIT", error))
      return fail("cannot release unbounded admission check");
    out->status = "reserved";
    return true;
  }
  WorkflowBudgetSnapshot current;
  if (!snapshot(db_, task.workflow_id, &current))
    return fail("cannot inspect durable workflow budget");
  out->charged = current.charged;
  out->held = current.held;
  for (int i = 0; i < 4; i++) {
    if (!limits[i] || !demand[i])
      continue;
    const auto available = std::max<int64_t>(
        0, limits[i] -
               std::min(limits[i], add(current.charged[i], current.held[i])));
    out->granted[i] = std::min(demand[i], available);
    if (out->granted[i] < minimum[i]) {
      out->dimension = i;
      out->status = current.held[i] > 0 && current.charged[i] < limits[i]
                        ? "waiting"
                        : "exhausted";
      exec_locked("ROLLBACK", nullptr);
      return true;
    }
  }
  unsigned char random[16];
  sqlite3_randomness(sizeof(random), random);
  const char *hex = "0123456789abcdef";
  for (auto byte : random) {
    out->token.push_back(hex[byte >> 4]);
    out->token.push_back(hex[byte & 15]);
  }
  auto s = query(db_, "INSERT INTO "
                      "workflow_budget_reservations(workflow_id,task_id,"
                      "attempt,token,state,tools,steps,elapsed_ms,tokens,"
                      "created_unix_ms) VALUES(?,?,?,?,'reserved',?,?,?,?,?)");
  bool ok = s && agent_db_bind_text(s.get(), 1, task.workflow_id) &&
            agent_db_bind_text(s.get(), 2, task.task_id) &&
            agent_db_bind_i32(s.get(), 3, task.attempt) &&
            agent_db_bind_text(s.get(), 4, out->token);
  for (int i = 0; ok && i < 4; i++)
    ok = agent_db_bind_i64(s.get(), 5 + i, out->granted[i]);
  if (!ok || !agent_db_bind_i64(s.get(), 9, agent_db_unix_ms_now()) ||
      !agent_db_step_done(s.get()))
    return fail(
        "workflow attempt already reserved or reservation persistence failed");
  if (!exec_locked("COMMIT", error))
    return fail("cannot commit workflow budget reservation");
  out->status = "reserved";
  return true;
#endif
}

bool AgentDb::settle_workflow_attempt(const WorkflowTaskRow &expected,
                                      const WorkflowTaskRow &updated,
                                      const std::string &token,
                                      std::string *error) {
  if (error)
    error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected;
  (void)updated;
  (void)token;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  if (expected.workflow_id != updated.workflow_id ||
      expected.task_id != updated.task_id ||
      expected.attempt != updated.attempt || updated.status == "running")
    return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN IMMEDIATE", error))
    return false;
  const auto fail = [&](const std::string &reason) {
    exec_locked("ROLLBACK", nullptr);
    if (error)
      *error = reason;
    return false;
  };
  {
    auto s = query(db_, "SELECT token,state FROM workflow_budget_reservations "
                        "WHERE workflow_id=? AND task_id=? AND attempt=?");
    if (!s || !agent_db_bind_text(s.get(), 1, expected.workflow_id) ||
        !agent_db_bind_text(s.get(), 2, expected.task_id) ||
        !agent_db_bind_i32(s.get(), 3, expected.attempt))
      return fail("cannot load workflow reservation");
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW) {
      if (token.empty() ||
          token !=
              reinterpret_cast<const char *>(sqlite3_column_text(s.get(), 0)) ||
          std::string(reinterpret_cast<const char *>(
              sqlite3_column_text(s.get(), 1))) != "reserved")
        return fail("stale reservation settlement rejected");
    } else if (state != SQLITE_DONE || !token.empty())
      return fail("reservation unavailable for settlement");
  }
  {
    auto s = query(
        db_, "UPDATE workflow_tasks SET "
             "status=?,updated_unix_ms=?,finished_unix_ms=?,ready_unix_ms=?,"
             "error=?,result_json=?,tool_calls_total_cum=MAX(tool_calls_total_"
             "cum,?),steps_executed_cum=MAX(steps_executed_cum,?),elapsed_ms_"
             "cum=MAX(elapsed_ms_cum,?),prompt_tokens_cum=MAX(prompt_tokens_"
             "cum,?),completion_tokens_cum=MAX(completion_tokens_cum,?),total_"
             "tokens_cum=MAX(total_tokens_cum,?) WHERE workflow_id=? AND "
             "task_id=? AND attempt=? AND request_json=? AND status='running'");
    if (!s || !agent_db_bind_text(s.get(), 1, updated.status) ||
        !agent_db_bind_i64(s.get(), 2, updated.updated_unix_ms) ||
        !agent_db_bind_i64(s.get(), 3, updated.finished_unix_ms) ||
        !agent_db_bind_i64(s.get(), 4, updated.ready_unix_ms) ||
        !agent_db_bind_text(s.get(), 5, updated.error) ||
        !agent_db_bind_text(s.get(), 6, updated.result_json))
      return fail("cannot bind workflow settlement");
    const int64_t values[] = {
        updated.tool_calls_total_cum,  updated.steps_executed_cum,
        updated.elapsed_ms_cum,        updated.prompt_tokens_cum,
        updated.completion_tokens_cum, updated.total_tokens_cum};
    for (int i = 0; i < 6; i++)
      if (!agent_db_bind_i64(s.get(), 7 + i, std::max<int64_t>(0, values[i])))
        return fail("cannot bind workflow usage");
    if (!bind_expected(s.get(), 13, expected) || !agent_db_step_done(s.get()) ||
        sqlite3_changes(db_) != 1)
      return fail("stale task settlement rejected");
  }
  if (!token.empty()) {
    auto s =
        query(db_, "UPDATE workflow_budget_reservations SET "
                   "state='settled',settled_unix_ms=? WHERE workflow_id=? AND "
                   "task_id=? AND attempt=? AND token=? AND state='reserved'");
    if (!s || !agent_db_bind_i64(s.get(), 1, agent_db_unix_ms_now()) ||
        !agent_db_bind_text(s.get(), 2, expected.workflow_id) ||
        !agent_db_bind_text(s.get(), 3, expected.task_id) ||
        !agent_db_bind_i32(s.get(), 4, expected.attempt) ||
        !agent_db_bind_text(s.get(), 5, token) ||
        !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1)
      return fail("reservation settlement conflict");
  }
  if (!exec_locked("COMMIT", error))
    return fail("cannot commit workflow settlement");
  return true;
#endif
}

bool AgentDb::mark_workflow_running(const std::string &id, int64_t now,
                                    std::string *error) {
  if (error)
    error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id;
  (void)now;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_)
    return false;
  auto s = query(
      db_, "UPDATE workflows SET status='running',updated_unix_ms=? WHERE "
           "workflow_id=? AND status='queued' AND cancel_requested=0 AND "
           "(COALESCE(deadline_unix_ms,0)=0 OR deadline_unix_ms>?)");
  return s && agent_db_bind_i64(s.get(), 1, now) &&
         agent_db_bind_text(s.get(), 2, id) &&
         agent_db_bind_i64(s.get(), 3, now) && agent_db_step_done(s.get()) &&
         sqlite3_changes(db_) == 1;
#endif
}

bool AgentDb::yield_workflow_task_capacity(const WorkflowTaskRow &expected,
                                           int64_t ready_ms,
                                           std::string *error) {
  if (error)
    error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected;
  (void)ready_ms;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_)
    return false;
  auto s =
      query(db_, "UPDATE workflow_tasks SET "
                 "status='queued',attempt=MAX(0,attempt-1),ready_unix_ms=?,"
                 "updated_unix_ms=? WHERE workflow_id=? AND task_id=? AND "
                 "attempt=? AND request_json=? AND status='running' AND NOT "
                 "EXISTS(SELECT 1 FROM workflow_budget_reservations r WHERE "
                 "r.workflow_id=workflow_tasks.workflow_id AND "
                 "r.task_id=workflow_tasks.task_id AND "
                 "r.attempt=workflow_tasks.attempt AND r.state='reserved')");
  if (!s || !agent_db_bind_i64(s.get(), 1, ready_ms) ||
      !agent_db_bind_i64(s.get(), 2, agent_db_unix_ms_now()) ||
      !bind_expected(s.get(), 3, expected) || !agent_db_step_done(s.get()) ||
      sqlite3_changes(db_) != 1) {
    if (error)
      *error = "capacity yield conflict";
    return false;
  }
  return true;
#endif
}

bool AgentDb::get_workflow_budget_snapshot(const std::string &id,
                                           WorkflowBudgetSnapshot *out,
                                           std::string *error) {
  if (error)
    error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id;
  (void)out;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  if (!out || id.empty())
    return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN", error))
    return false;
  if (!snapshot(db_, id, out)) {
    exec_locked("ROLLBACK", nullptr);
    if (error)
      *error = "cannot load workflow budget";
    return false;
  }
  if (!exec_locked("COMMIT", error)) {
    exec_locked("ROLLBACK", nullptr);
    return false;
  }
  return true;
#endif
}
} // namespace agentd
