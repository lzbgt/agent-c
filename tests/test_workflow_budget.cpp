#include "agent_db.h"
#include "workflow_budget.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace agentd;
namespace {
using Amounts = AgentDb::WorkflowBudgetAmounts;
void check(bool ok, const std::string &message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void sql(const std::string &path, const char *text) {
  sqlite3 *db = nullptr;
  check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture open");
  check(sqlite3_exec(db, text, nullptr, nullptr, nullptr) == SQLITE_OK,
        "fixture SQL");
  sqlite3_close(db);
}
AgentDb::WorkflowRow workflow(AgentDb &db, const std::string &id) {
  AgentDb::WorkflowRow wf;
  wf.workflow_id = id;
  wf.status = "queued";
  wf.spec_json = "{}";
  std::vector<AgentDb::WorkflowTaskRow> rows;
  for (const auto *id : {"a", "b", "c"}) {
    AgentDb::WorkflowTaskRow row;
    row.workflow_id = wf.workflow_id;
    row.task_id = id;
    row.status = "queued";
    row.request_json = "{}";
    row.depends_on_json = "[]";
    row.max_attempts = 4;
    rows.push_back(row);
  }
  std::string error;
  check(db.create_workflow(wf, rows, &error), error);
  return wf;
}
AgentDb::WorkflowTaskRow claim(AgentDb &db, const AgentDb::WorkflowRow &wf,
                               const std::string &id, int attempt = 1) {
  std::string error;
  check(db.claim_workflow_task_budgeted(wf.workflow_id, id, 0, attempt, 4, 0,
                                        "", &error),
        error);
  std::vector<AgentDb::WorkflowTaskRow> rows;
  check(db.list_workflow_tasks(wf.workflow_id, &rows, &error), error);
  for (const auto &row : rows)
    if (row.task_id == id)
      return row;
  check(false, "claimed row absent");
  return {};
}
AgentDb::WorkflowBudgetGrant reserve(AgentDb &db,
                                     const AgentDb::WorkflowRow &wf,
                                     const AgentDb::WorkflowTaskRow &task,
                                     const Amounts &limits,
                                     const Amounts &demand) {
  Amounts minimum{};
  for (int i = 0; i < 4; i++)
    minimum[i] = demand[i] > 0 ? 1 : 0;
  AgentDb::WorkflowBudgetGrant out;
  std::string error;
  check(db.reserve_workflow_budget(wf, task, limits, demand, minimum, &out,
                                   &error),
        error);
  return out;
}
AgentDb::WorkflowBudgetSnapshot snapshot(AgentDb &db, const std::string &id) {
  AgentDb::WorkflowBudgetSnapshot out;
  std::string error;
  check(db.get_workflow_budget_snapshot(id, &out, &error), error);
  return out;
}
AgentDb::WorkflowTaskRow finish(const AgentDb::WorkflowTaskRow &task,
                                const Amounts &actual) {
  auto out = task;
  out.status = "done";
  out.tool_calls_total_cum += actual[0];
  out.steps_executed_cum += actual[1];
  out.elapsed_ms_cum += actual[2];
  out.total_tokens_cum += actual[3];
  out.result_json = "{\"ok\":true}";
  return out;
}
} // namespace
int main() {
  workflow_engine_internal::WorkflowRunCancelCtx context;
  context.budget_deadline_unix_ms = 1;
  check(workflow_engine_internal::workflow_run_should_cancel(&context) &&
            context.reason == workflow_engine_internal::WorkflowCancelReason::
                                  BudgetGrantElapsed,
        "attempt grant expiry became an owner/global stop");
  DaemonConfig config;
  Json::Value malformed;
  malformed["kind"] = 5;
  malformed["tools"] = Json::Value(Json::arrayValue);
  const Amounts bounds{4, 4, 100, 100};
  check(workflow_budget_demand(malformed, config, bounds)[3] == 100,
        "unknown model kind escaped token reservation");
  malformed["kind"] = "http_json";
  malformed["http_json"] = "invalid";
  check(workflow_budget_demand(malformed, config, bounds)[0] == 1,
        "invalid HTTP input crashed demand inspection");
  const auto root =
      std::filesystem::temp_directory_path() /
      ("agent-budget-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.db").string();
  AgentDb first, second;
  std::string error;
  check(first.open(path, &error), error);
  check(second.open(path, &error), error);
  const Amounts limits{4, 4, 100, 100};
  auto wf = workflow(first, "parallel");
  auto a = claim(first, wf, "a"), b = claim(second, wf, "b");
  std::atomic<bool> go{false};
  AgentDb::WorkflowBudgetGrant grants[2];
  std::thread one([&] {
    while (!go.load())
      std::this_thread::yield();
    grants[0] = reserve(first, wf, a, limits, limits);
  });
  std::thread two([&] {
    while (!go.load())
      std::this_thread::yield();
    grants[1] = reserve(second, wf, b, limits, limits);
  });
  go = true;
  one.join();
  two.join();
  int winner = grants[0].status == "reserved" ? 0 : 1;
  check(grants[winner].status == "reserved" &&
            grants[1 - winner].status == "waiting",
        "parallel attempts double-spent capacity");
  auto active = snapshot(first, wf.workflow_id);
  check(active.held == limits && active.active_attempts == 1,
        "held capacity incorrect");
  auto &owned = winner == 0 ? a : b;
  auto &waiting = winner == 0 ? b : a;
  check(first.yield_workflow_task_capacity(waiting, 0, &error), error);
  auto used = finish(owned, {1, 1, 10, 20});
  check(!first.settle_workflow_attempt(owned, used, "wrong-token", &error),
        "wrong grant settled");
  check(snapshot(first, wf.workflow_id).held == limits,
        "failed settlement refunded grant");
  // Failure between task persistence and reservation release rolls both back.
  sql(path, "CREATE TRIGGER fail_settlement BEFORE UPDATE ON "
            "workflow_budget_reservations WHEN NEW.state='settled' BEGIN "
            "SELECT RAISE(ABORT,'injected settlement failure'); END;");
  check(
      !first.settle_workflow_attempt(owned, used, grants[winner].token, &error),
      "injected settlement succeeded");
  check(snapshot(first, wf.workflow_id).charged == Amounts{} &&
            snapshot(first, wf.workflow_id).held == limits,
        "partial settlement exposed capacity");
  sql(path, "DROP TRIGGER fail_settlement;");
  check(
      first.settle_workflow_attempt(owned, used, grants[winner].token, &error),
      error);
  check(!second.settle_workflow_attempt(owned, used, grants[winner].token,
                                        &error),
        "duplicate settlement charged twice");
  waiting = claim(second, wf, waiting.task_id);
  check(waiting.attempt == 1, "capacity wait consumed an attempt");
  auto next = reserve(second, wf, waiting, limits, limits);
  check(next.granted == Amounts({3, 3, 90, 80}), "unused grant not refunded");
  check(second.settle_workflow_attempt(waiting, finish(waiting, {3, 3, 90, 80}),
                                       next.token, &error),
        error);
  auto c = claim(first, wf, "c");
  auto empty = reserve(first, wf, c, limits, limits);
  check(empty.status == "exhausted" && empty.dimension == 0,
        "spent budget admitted work");
  // Modest caps permit concurrency rather than serializing every workflow.
  wf = workflow(first, "bounded");
  a = claim(first, wf, "a");
  b = claim(second, wf, "b");
  auto ga = reserve(first, wf, a, limits, {1, 1, 10, 10});
  auto gb = reserve(second, wf, b, limits, {1, 1, 10, 10});
  check(ga.status == "reserved" && gb.status == "reserved" &&
            snapshot(first, wf.workflow_id).active_attempts == 2,
        "bounded independent attempts did not share capacity");
  // A stop cannot be cleared by a stale scheduler's running transition.
  wf.cancel_requested = true;
  check(first.upsert_workflow(wf, &error), error);
  check(!second.mark_workflow_running(wf.workflow_id, 1, &error),
        "scheduler cleared owner stop");
  c = claim(second, wf, "c");
  check(reserve(second, wf, c, limits, limits).status == "stopped",
        "stopped workflow obtained grant");
  // Crash recovery conservatively charges open grants and rejects their old
  // holders.
  check(first.recover_inflight_workflows(0, &error), error);
  auto recovered = snapshot(second, "bounded");
  check(recovered.held == Amounts{} &&
            recovered.uncertain == Amounts({2, 2, 20, 20}) &&
            recovered.uncertain_attempts == 2,
        "recovery refunded uncertain spend");
  check(!first.settle_workflow_attempt(a, finish(a, {1, 1, 1, 1}), ga.token,
                                       &error),
        "stale worker settled after recovery");
  wf = workflow(first, "overrun");
  a = claim(first, wf, "a");
  ga = reserve(first, wf, a, limits, {1, 1, 10, 10});
  check(first.settle_workflow_attempt(a, finish(a, {2, 1, 15, 30}), ga.token,
                                      &error),
        error);
  check(snapshot(first, wf.workflow_id).charged == Amounts({2, 1, 15, 30}),
        "actual overage hidden by grant clipping");
  // Fairness counts and claims serialize across SQLite connections too.
  wf = workflow(first, "fairness");
  std::atomic<int> claimed{0};
  go = false;
  one = std::thread([&] {
    while (!go)
      std::this_thread::yield();
    if (first.claim_workflow_task_budgeted(wf.workflow_id, "a", 0, 1, 1, 0, "",
                                           nullptr))
      claimed++;
  });
  two = std::thread([&] {
    while (!go)
      std::this_thread::yield();
    if (second.claim_workflow_task_budgeted(wf.workflow_id, "b", 0, 1, 1, 0, "",
                                            nullptr))
      claimed++;
  });
  go = true;
  one.join();
  two.join();
  check(claimed == 1, "parallel claims exceeded fairness cap");
  wf = workflow(first, "unbounded");
  a = claim(first, wf, "a");
  ga = reserve(first, wf, a, Amounts{}, Amounts{});
  check(ga.status == "reserved" && ga.token.empty() &&
            snapshot(first, wf.workflow_id).active_attempts == 0,
        "unbounded default created a useless grant journal");
  check(first.settle_workflow_attempt(a, finish(a, {1, 1, 10, 20}), ga.token,
                                      &error),
        error);
  check(snapshot(first, wf.workflow_id).charged == Amounts({1, 1, 10, 20}),
        "unbounded attempt lost observed usage");
  first.close();
  second.close();
#if !defined(_WIN32)
  // Kill a real owner at both sides of atomic settlement.
  for (bool settle_before_kill : {false, true}) {
    const auto crash =
        (root / (settle_before_kill ? "settled.db" : "open.db")).string();
    {
      AgentDb init;
      check(init.open(crash, &error), error);
      workflow(init, "crash");
    }
    int ready[2];
    check(pipe(ready) == 0, "pipe");
    const pid_t child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
      close(ready[0]);
      AgentDb db;
      if (!db.open(crash, nullptr))
        _exit(2);
      AgentDb::WorkflowRow state;
      db.get_workflow("crash", &state, nullptr);
      auto task = claim(db, state, "a");
      auto grant = reserve(db, state, task, limits, limits);
      if (settle_before_kill &&
          !db.settle_workflow_attempt(task, finish(task, {1, 1, 10, 20}),
                                      grant.token, nullptr))
        _exit(3);
      char byte = 'R';
      write(ready[1], &byte, 1);
      for (;;)
        pause();
    }
    close(ready[1]);
    char byte = 0;
    check(read(ready[0], &byte, 1) == 1 && byte == 'R',
          "writer did not persist grant");
    close(ready[0]);
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFSIGNALED(status), "writer not killed");
    AgentDb db;
    check(db.open(crash, &error), error);
    check(db.recover_inflight_workflows(0, &error), error);
    auto state = snapshot(db, "crash");
    check(state.held == Amounts{}, "dead owner retained ordinary capacity");
    check(state.charged ==
              (settle_before_kill ? Amounts({1, 1, 10, 20}) : limits),
          "crash settlement lost or doubled charge");
    check(state.uncertain_attempts == (settle_before_kill ? 0 : 1),
          "settled usage became uncertain");
  }
#endif
  std::filesystem::remove_all(root);
  std::cout << "workflow budget tests passed\n";
  return 0;
}
