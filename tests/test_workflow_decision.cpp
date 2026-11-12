#include "workflow_decision.h"
#include "agentd/api.h"
#include "workflow_budget.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <thread>

namespace {
void check(bool ok, const std::string& message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
Json::Value spec() {
  Json::Value value;
  value["prompt"] = "Release this batch?";
  value["choices"][0]["id"] = "release"; value["choices"][0]["label"] = "Release"; value["choices"][0]["outcome"] = "continue";
  value["choices"][1]["id"] = "reject"; value["choices"][1]["label"] = "Reject"; value["choices"][1]["outcome"] = "fail";
  return value;
}
void sql(const std::string& path, const std::string& statement) {
  sqlite3* db = nullptr; check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture open");
  sqlite3_busy_timeout(db, 1000);
  check(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL: " + statement);
  sqlite3_close(db);
}
agentd::AgentDb::WorkflowTaskRow task(agentd::AgentDb& db, const std::string& id) {
  std::vector<agentd::AgentDb::WorkflowTaskRow> rows; std::string error;
  check(db.list_workflow_tasks("review", &rows, &error), error);
  for (const auto& row : rows) if (row.task_id == id) return row;
  check(false, "task missing"); return {};
}
}
int main() {
  using namespace agentd;
  using Result = AgentDb::WorkflowDecisionResolution;
  const auto root = std::filesystem::temp_directory_path() / ("agent-decision-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db; std::string error;
  check(db.open(path, &error), error);
  db.close(); sql(path, "DROP TABLE workflow_decisions; UPDATE meta SET value='37' WHERE key='schema_version';");
  check(db.open(path, &error), error);
  AgentDb::WorkflowRow wf; wf.workflow_id = "review"; wf.status = "running"; wf.spec_json = "{}";
  wf.created_unix_ms = wf.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const auto* id : {"choice", "timeout", "stop", "tamper", "race"}) {
    AgentDb::WorkflowTaskRow t; t.workflow_id = wf.workflow_id; t.task_id = id;
    t.status = "running"; t.attempt = 1; t.max_attempts = 3;
    t.request_json = "{}"; t.depends_on_json = "[]"; t.created_unix_ms = t.updated_unix_ms = 1;
    tasks.push_back(t);
  }
  check(db.create_workflow(wf, tasks, &error), error);
  check(workflow_decision_validate(spec(), &error), error);
  auto invalid = spec(); invalid["choices"][1]["id"] = "release";
  check(!workflow_decision_validate(invalid, &error), "duplicate choice accepted");
  invalid = spec(); invalid["choices"][0]["outcome"] = "cancel-all";
  check(!workflow_decision_validate(invalid, &error), "unknown outcome accepted");
  invalid = spec(); invalid["actor"] = "owner";
  check(!workflow_decision_validate(invalid, &error), "client identity accepted");
  invalid = spec(); invalid["timeout_ms"] = Json::Int64(2592000001LL);
  check(!workflow_decision_validate(invalid, &error), "oversized timeout accepted");
  invalid = spec(); invalid["prompt"] = std::string("bad\0prompt", 10);
  check(!workflow_decision_validate(invalid, &error), "NUL prompt accepted");

  bool yielded = false;
  auto output = workflow_decision_step(&db, task(db, "choice"), spec(), &yielded);
  check(yielded && output.isNull(), "decision did not yield");
  auto waiting = task(db, "choice");
  check(waiting.status == "queued" && waiting.attempt == 0, "wait occupied an attempt or worker");
  Json::Value projection;
  check(workflow_decisions_projection(&db, wf, &projection, &error) && projection.size() == 1 && projection[0]["available"] == true, error);
  AgentDb::WorkflowDecisionRow row, saved;
  check(db.get_workflow_decision("review", "choice", &row, &error), error);
  const auto identity = row.decision_id;
  db.close(); check(db.open(path, &error), error);
  check(db.get_workflow_decision("review", "choice", &saved, &error) && saved.decision_id == identity && saved.state == "waiting", "wait lost across restart");
  check(db.resolve_workflow_decision(row, "release", "continue", "checked", "delivery-1", &saved, &error) == Result::Recorded, error);
  const auto ready = task(db, "choice").ready_unix_ms;
  check(db.resolve_workflow_decision(row, "release", "continue", "checked", "delivery-1", &saved, &error) == Result::Replayed, error);
  check(task(db, "choice").ready_unix_ms == ready, "replay woke task again");
  check(db.resolve_workflow_decision(row, "reject", "fail", "", "delivery-2", &saved, &error) == Result::Conflict, "second choice accepted");
  check(db.resolve_workflow_decision(row, "release", "continue", "changed", "delivery-1", &saved, &error) == Result::Conflict, "delivery payload changed");
  // Simulate a crash after receipt, then the normal re-claim. No second input needed.
  db.close(); check(db.open(path, &error), error);
  sql(path, "UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='choice';");
  output = workflow_decision_step(&db, task(db, "choice"), spec(), &yielded);
  check(!yielded && output["ok"] == true && output["choice_id"] == "release" && output["elapsed_ms"] == 0, output.toStyledString());

  auto timed = spec(); timed["timeout_ms"] = 1000;
  output = workflow_decision_step(&db, task(db, "timeout"), timed, &yielded);
  check(yielded, "timed decision did not wait");
  check(db.get_workflow_decision("review", "timeout", &row, &error) && row.expires_unix_ms > row.created_unix_ms, "timeout missing");
  sql(path, "UPDATE workflow_decisions SET expires_unix_ms=1 WHERE task_id='timeout'; UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='timeout';");
  output = workflow_decision_step(&db, task(db, "timeout"), timed, &yielded);
  check(!yielded && output["ok"] == false && output["retryable"] == false && output["decision"]["state"] == "expired", "timeout retried or continued");

  output = workflow_decision_step(&db, task(db, "race"), spec(), &yielded);
  check(yielded && db.get_workflow_decision("review", "race", &row, &error), error);
  AgentDb other; check(other.open(path, &error), error);
  Result first = Result::StorageError, second = Result::StorageError;
  std::thread a([&] { AgentDb::WorkflowDecisionRow receipt; std::string e; first = db.resolve_workflow_decision(row, "release", "continue", "", "race-a", &receipt, &e); });
  std::thread b([&] { AgentDb::WorkflowDecisionRow receipt; std::string e; second = other.resolve_workflow_decision(row, "reject", "fail", "", "race-b", &receipt, &e); });
  a.join(); b.join(); other.close();
  check((first == Result::Recorded && second == Result::Conflict) || (second == Result::Recorded && first == Result::Conflict), "choice race did not select exactly one immutable receipt");

  output = workflow_decision_step(&db, task(db, "tamper"), spec(), &yielded);
  check(yielded, "tamper fixture did not wait");
  sql(path, "UPDATE workflow_decisions SET spec_json='{}' WHERE task_id='tamper'; UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='tamper';");
  output = workflow_decision_step(&db, task(db, "tamper"), spec(), &yielded);
  check(!yielded && output["ok"] == false && output["retryable"] == false, "tampered decision resumed");

  output = workflow_decision_step(&db, task(db, "stop"), spec(), &yielded);
  check(yielded && db.get_workflow_decision("review", "stop", &row, &error), error);
  sql(path, "UPDATE workflows SET cancel_requested=1 WHERE workflow_id='review';");
  check(db.resolve_workflow_decision(row, "release", "continue", "", "stop-answer", &saved, &error) == Result::Stopped, "owner stop bypassed");
  check(task(db, "stop").attempt == 0, "cancelled wait consumed attempt");
  check(db.get_workflow_decision("review", "choice", &row, &error), error);
  check(db.resolve_workflow_decision(row, "release", "continue", "checked", "delivery-1", &saved, &error) == Result::Replayed, "historical receipt unavailable after stop");
  AgentDb::WorkflowRow stopped; check(db.get_workflow("review", &stopped, &error) && stopped.cancel_requested, "replay cleared owner stop");

  Json::Value request; request["kind"] = "decision";
  check(workflow_budget_demand(request, DaemonConfig(), {10, 10, 10000, 100}) == AgentDb::WorkflowBudgetAmounts{}, "decision reserved execution budget");
  db.close();
  {
    AgentdApi::Options options;
    options.cfg.db_path = (root / "facade.sqlite").string(); options.cfg.state_dir = (root / "facade").string();
    options.cfg.auth_token = "decision-facade-token";
    AgentdApi api(options); check(api.init(&error), error);
    HttpRequest req; req.method = "POST"; req.path = "/api/v1/workflow/decision"; req.body = "{}";
    HttpResponse resp;
    check(api.handle(req, &resp) && resp.status == 401, "facade decision route must require auth");
    req.headers["authorization"] = "Bearer decision-facade-token"; resp = {};
    check(api.handle(req, &resp) && resp.status == 400, "facade decision route missing");
  }
  std::filesystem::remove_all(root);
  std::cout << "Decision migration, validation, wait/wake, immutable race, restart, timeout, stop and auth checks passed\n";
}
