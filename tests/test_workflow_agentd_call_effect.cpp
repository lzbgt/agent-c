#include "workflow_agentd_call.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>

namespace {
void check(bool condition, const std::string& message) {
  if (!condition) { std::cerr << message << '\n';std::exit(1); }
}
Json::Value call() {
  Json::Value value;value["base_url"] = "http://127.0.0.1:12345";
  value["workflow"]["allow_sessions"] = true;
  value["workflow"]["session_id"] = "scope &/parent";
  value["workflow"]["tasks"][0]["task_id"] = "child";
  value["workflow"]["tasks"][0]["kind"] = "delay";
  value["workflow"]["tasks"][0]["delay_ms"] = 1;
  value["workflow"]["trace_id"] = "caller-trace";value["workflow"]["idempotency_key"] = "caller-key";
  return value;
}
Json::Value remote(const agentd::WorkflowAgentdCallEffect& effect, const std::string& state = "done") {
  Json::Value value;value["ok"] = true;
  value["workflow"]["workflow_id"] = "original-remote";value["workflow"]["session_id"] = "scope &/parent";
  value["workflow"]["idempotency_key"] = effect.remote_key();value["workflow"]["trace_id"] = effect.remote_key();
  value["workflow"]["status"] = state;value["result"]["output"] = "original output";return value;
}
Json::Value result(const agentd::WorkflowAgentdCallEffect& effect, bool ok = true) {
  Json::Value value;value["ok"] = ok;value["assistant_text"] = "saved output";
  value["agentd"]["final"] = remote(effect, ok ? "done" : "error");
  value["agentd"]["workflow_id"] = "original-remote";
  value["tool_calls_total"] = 3;value["steps_executed"] = 3;return value;
}
}
int main() {
  using namespace agentd;
  const auto root = std::filesystem::temp_directory_path() /
    ("remote-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "parent.sqlite").string();
  AgentDb db;std::string error;
  check(db.open(path, &error), error);
  AgentDb::WorkflowRow workflow;workflow.workflow_id = "chosen-parent-name";workflow.status = "queued";workflow.spec_json = "{}";
  workflow.created_unix_ms = workflow.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const auto* id : {"saved","error","unknown","identity","corrupt","race","stop","admit-failure","receipt-failure","pending","legacy","corrupt-complete","oversized"}) {
    AgentDb::WorkflowTaskRow task;task.workflow_id = workflow.workflow_id;task.task_id = id;task.status = "queued";
    task.request_json = "{}";task.depends_on_json = "[]";task.created_unix_ms = task.updated_unix_ms = 1;tasks.push_back(task);
  }
  check(db.create_workflow(workflow, tasks, &error), error);
  auto context = [&](const std::string& id) { return WorkflowEffectContext{&db, workflow.workflow_id, id}; };
  auto sql = [&](const std::string& statement) {
    sqlite3* raw = nullptr;check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "fixture open");
    check(sqlite3_exec(raw, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(raw));sqlite3_close(raw);
  };
  std::string saved_key;
  {
    WorkflowAgentdCallEffect effect(context("saved"), call());check(effect.preflight().isNull(), "initial probe");
    std::vector<AgentDb::WorkflowEffectRow> rows;
    check(db.list_workflow_effects(workflow.workflow_id, &rows, &error) && rows.empty(), "preflight admitted execution");
    check(effect.remote_key().size() == 71 && effect.remote_key().rfind("remote_",0) == 0, "unbounded/nonowned remote key");
    check(effect.submission()["idempotency_key"] != "caller-key" && effect.submission()["trace_id"] == effect.remote_key(), "caller controls remote identity");
    saved_key = effect.remote_key();check(effect.admit(&error) && !effect.needs_submission(), error);
    check(!effect.admit(&error), "second submit admitted");
    check(effect.observe_workflow(remote(effect, "running"), &error), error);
    auto changed = remote(effect);changed["workflow"]["workflow_id"] = "replacement";
    check(!effect.matches_workflow(changed), "known remote identity drift accepted");
    const auto done = effect.complete(result(effect));
    check(done["ok"] == true && done["effect"]["state"] == "completed" && done["effect"]["attempt"] == 1, done.toStyledString());
  }
  db.close();check(db.open(path, &error), error);
  {
    WorkflowAgentdCallEffect effect(context("saved"), call());const auto replay = effect.preflight();
    check(effect.remote_key() == saved_key && replay["ok"] == true && replay["effect"]["replayed"] == true &&
      replay["tool_calls_total"] == 0 && replay["steps_executed"] == 0 && replay["effect"]["original_usage"]["tool_calls_total"] == 3,
      "reopened terminal receipt lost identity or charged new I/O");
    auto changed = call();changed["base_url"] = "http://127.0.0.1:23456";
    WorkflowAgentdCallEffect moved(context("saved"), changed);
    check(moved.preflight()["needs_input"] == true, "changed target reused receipt");
    changed = call();changed["workflow"]["tasks"][0]["delay_ms"] = 2;
    WorkflowAgentdCallEffect edited(context("saved"), changed);
    check(edited.preflight()["needs_input"] == true, "changed request reused receipt");
  }
  {
    AgentDb other;check(other.open((root / "other.sqlite").string(), &error), error);
    check(other.create_workflow(workflow, tasks, &error), error);
    WorkflowAgentdCallEffect second({&other, workflow.workflow_id, "saved"}, call());check(second.preflight().isNull(), "second authority");
    check(second.remote_key() != saved_key, "independent parents collide on chosen workflow/task names");other.close();
  }
  {
    WorkflowAgentdCallEffect effect(context("unknown"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    check(effect.observe_workflow(remote(effect, "running"), &error), error);
  }
  db.close();check(db.open(path, &error), error);
  {
    WorkflowAgentdCallEffect restored(context("unknown"), call());check(restored.preflight().isNull() && !restored.needs_submission(), "restart grants a replacement POST");
    auto changed = remote(restored);changed["workflow"]["workflow_id"] = "other";
    check(!restored.matches_workflow(changed), "restart lost pinned remote ID");
    check(restored.complete(result(restored))["ok"] == true, "original completion unavailable after restart");
  }
  {
    WorkflowAgentdCallEffect effect(context("identity"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    for (const auto* field : {"idempotency_key","trace_id","session_id","status"}) {
      auto changed = remote(effect);changed["workflow"][field] = "wrong";
      check(!effect.matches_workflow(changed), std::string("bad remote field accepted: ") + field);
    }
    auto unscoped = call();unscoped["workflow"]["allow_sessions"] = false;
    WorkflowAgentdCallEffect public_scope(context("pending"), unscoped);check(public_scope.preflight().isNull(), "unscoped probe");
    auto no_scope = remote(public_scope);no_scope["workflow"].removeMember("session_id");
    check(public_scope.matches_workflow(no_scope), "native no-session scope mismatch");
    no_scope["workflow"]["session_id"] = Json::Value(Json::arrayValue);
    check(!public_scope.matches_workflow(no_scope), "nonstring empty scope was accepted");
    for (const auto& malformed : {Json::Value(), Json::Value(Json::arrayValue), Json::Value("scalar")})
      check(!effect.matches_workflow(malformed), "malformed remote envelope accepted");
    check(effect.complete(Json::Value(Json::arrayValue))["needs_input"] == true, "invalid terminal result saved");
  }
  {
    WorkflowAgentdCallEffect effect(context("error"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    check(effect.complete(result(effect, false))["ok"] == false, "remote error promoted to success");
    WorkflowAgentdCallEffect restored(context("error"), call());const auto replay = restored.preflight();
    check(replay["ok"] == false && replay["retryable"] == false && replay["effect"]["replayed"] == true, "terminal remote error dispatched again");
  }
  {
    WorkflowAgentdCallEffect first(context("race"), call()), second(context("race"), call());
    check(first.preflight().isNull() && second.preflight().isNull(), "race probe");
    check(first.admit(&error) && !second.admit(&error), "concurrent runners both admitted a submit");
    WorkflowAgentdCallEffect winner(context("race"), call());check(winner.preflight().isNull(), "winner receipt read");
    check(winner.complete(result(winner))["ok"] == true, "winner completion");
    check(first.complete(result(first))["needs_input"] == true, "stale runner overwrote terminal receipt");
  }
  {
    WorkflowAgentdCallEffect effect(context("admit-failure"), call());check(effect.preflight().isNull(), "admission failure probe");
    sql("CREATE TRIGGER fail_remote_admission BEFORE UPDATE ON workflow_effects WHEN NEW.state='dispatching' BEGIN SELECT RAISE(ABORT,'fixture'); END");
    check(!effect.admit(&error), "failed persistence authorized submit");sql("DROP TRIGGER fail_remote_admission");
    WorkflowAgentdCallEffect retry(context("admit-failure"), call());check(retry.preflight().isNull() && retry.needs_submission(), "unadmitted preparation became uncertainty");
  }
  {
    WorkflowAgentdCallEffect effect(context("receipt-failure"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    sql("CREATE TRIGGER fail_remote_receipt BEFORE UPDATE ON workflow_effects WHEN NEW.state='completed' BEGIN SELECT RAISE(ABORT,'fixture'); END");
    check(effect.complete(result(effect))["needs_input"] == true, "failed receipt persistence reported success");sql("DROP TRIGGER fail_remote_receipt");
    WorkflowAgentdCallEffect restored(context("receipt-failure"), call());check(restored.preflight().isNull() && !restored.needs_submission(), "failed receipt save permits submit");
  }
  {
    WorkflowAgentdCallEffect effect(context("corrupt"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    sql("UPDATE workflow_effects SET receipt_json='{}' WHERE task_id='corrupt'");
    WorkflowAgentdCallEffect restored(context("corrupt"), call());check(restored.preflight()["needs_input"] == true, "corrupt admission recovered by guessing");
  }
  {
    bool stopped = false;auto ctx = context("stop");ctx.should_cancel = [&] { return stopped; };
    WorkflowAgentdCallEffect effect(ctx, call());check(effect.preflight().isNull(), "stop probe");stopped = true;
    check(!effect.admit(&error), "owner stop admitted submit");
    check(effect.preflight()["cancelled"] == true, "stopped parent revived");
    WorkflowAgentdCallEffect pending(context("pending"), call(), false);check(pending.preflight().isNull(), "pending probe");
    check(pending.pending("temporary lookup", true)["needs_input"] != true, "recoverable wait became an owner blocker");
    check(pending.pending("wrong identity", false)["needs_input"] == true, "unsafe receipt lacks scoped blocker");
  }
  {
    WorkflowAgentdCallEffect old(context("legacy"), call(), true, true);
    check(old.preflight()["needs_input"] == true && !old.admit(&error), "unrecorded earlier attempt became a fresh submit");
    WorkflowAgentdCallEffect effect(context("corrupt-complete"), call());check(effect.preflight().isNull() && effect.admit(&error), error);
    check(effect.complete(result(effect))["ok"] == true, "corruption setup");
    sql("UPDATE workflow_effects SET receipt_json=replace(receipt_json,'saved output','changed output') WHERE task_id='corrupt-complete'");
    WorkflowAgentdCallEffect damaged(context("corrupt-complete"), call());
    check(damaged.preflight()["needs_input"] == true, "corrupt terminal checksum trusted");
    WorkflowAgentdCallEffect large(context("oversized"), call());check(large.preflight().isNull() && large.admit(&error), error);
    auto oversized = result(large);oversized["agentd"]["final"]["result"]["output"] = std::string(8*1024*1024, 'x');
    check(large.complete(oversized)["needs_input"] == true, "oversized terminal result persisted");
    WorkflowAgentdCallEffect unavailable(context("oversized"), call());
    check(unavailable.preflight().isNull() && !unavailable.needs_submission(), "oversized receipt permits another POST");
    std::string space;check(db.workflow_remote_namespace(&space, &error), error);
    check(db.meta_set("workflow_remote_namespace_v1", "invalid", &error), error);
    WorkflowAgentdCallEffect invalid(context("saved"), call());check(invalid.preflight()["ok"] == false, "invalid namespace was silently replaced");
    std::string retained;check(db.meta_get("workflow_remote_namespace_v1", &retained, &error) && retained == "invalid", "namespace changed on failure");
    check(db.meta_set("workflow_remote_namespace_v1", space, &error), error);
  }
  db.close();std::filesystem::remove_all(root);
  std::cout << "Remote admission, scoped namespaces, receipts, CAS, restart, storage and stop checks passed\n";
}
