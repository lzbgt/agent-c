#include "workflow_model_effect.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>

namespace {
void check(bool value, const std::string& reason) {
  if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
struct Fake {
  int calls = 0;
  std::string result = R"({"ok":true,"data":{"output":"original"}})";
  bool throws = false;
  agent_status_t status = AGENT_OK;
  agent_tool_executor_t executor() {
    return {this, [](void* ptr, const char*, const char*, agent_string_t* out) {
      auto& self = *static_cast<Fake*>(ptr); ++self.calls;
      if (self.throws) throw std::runtime_error("lost executor");
      if (self.status != AGENT_OK) return self.status;
      return agent_string_set_copy(out, self.result.data(), self.result.size());
    }};
  }
};
agent_status_t invoke(agent_tool_executor_t executor) {
  agent_string_t result{};
  auto status = executor.execute(executor.ctx, "proc_exec", "{}", &result);
  agent_string_free(&result); return status;
}
Json::Value success() {
  Json::Value out; out["ok"] = true; out["assistant_text"] = "saved answer";
  out["steps_executed"] = 2; out["total_tokens"] = 17; return out;
}
}
int main() {
  using namespace agentd;
  const auto root = std::filesystem::temp_directory_path() /
    ("model-effect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db; std::string error;
  check(db.open(path, &error), error);
  AgentDb::WorkflowRow wf;
  wf.workflow_id = "wf"; wf.status = "queued"; wf.spec_json = "{}";
  wf.created_unix_ms = wf.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const char* id : {"pure", "saved", "crash", "provider", "invalid", "large", "throw", "timeout", "negative",
                         "status", "capacity", "stop", "race", "admission", "receipt", "nonzero"}) {
    AgentDb::WorkflowTaskRow task;
    task.workflow_id = "wf"; task.task_id = id; task.status = "queued";
    task.request_json = "{}"; task.depends_on_json = "[]";
    task.created_unix_ms = task.updated_unix_ms = 1; tasks.push_back(task);
  }
  check(db.create_workflow(wf, tasks, &error), error);
  const std::string request = R"({"prompt":"work","tools":"host","max_steps":10})";
  auto context = [&](const char* id) { return WorkflowEffectContext{&db, "wf", id}; };
  auto sql = [&](const char* text) {
    sqlite3* raw = nullptr;
    check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "fixture open failed");
    check(sqlite3_exec(raw, text, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(raw));
    sqlite3_close(raw);
  };
  {
    WorkflowModelEffect guard(context("pure"), request);
    check(guard.preflight().isNull(), "pure preflight failed");
    Json::Value failed; failed["ok"] = false; failed["retryable"] = true;
    check(guard.finish(failed)["retryable"] == true, "provider-only retry was suppressed");
    std::vector<AgentDb::WorkflowEffectRow> rows;
    check(db.list_workflow_effects("wf", &rows, &error) && rows.empty(), "read-only preflight wrote a journal row");
  }
  Fake fake;
  {
    WorkflowModelEffect guard(context("saved"), request);
    check(guard.preflight().isNull(), "saved preflight");
    auto executor = guard.wrap(fake.executor());
    check(invoke(executor) == AGENT_OK && invoke(executor) == AGENT_OK, "valid tools failed");
    auto out = guard.finish(success());
    check(out["ok"] == true && out["effect"]["state"] == "completed" && out["tool_calls_total"] == 2, out.toStyledString());
    const auto calls = fake.calls;
    check(invoke(executor) != AGENT_OK && fake.calls == calls, "finished run admitted another tool");
  }
  {
    WorkflowModelEffect replay(context("saved"), request);
    auto out = replay.preflight();
    check(out["ok"] == true && out["effect"]["replayed"] == true && out["assistant_text"] == "saved answer" &&
      out["tool_calls_total"] == 0 && out["steps_executed"] == 0 && out["total_tokens"] == 0 &&
      out["effect"]["original_usage"]["tool_calls_total"] == 2 && fake.calls == 2, "saved receipt charged or executed again");
    WorkflowModelEffect changed(context("saved"), R"({"prompt":"different","tools":"host"})");
    check(changed.preflight()["needs_input"] == true, "changed request reused completed identity");
  }
  {
    WorkflowModelEffect guard(context("crash"), request);
    check(guard.preflight().isNull(), "crash preflight");
    check(invoke(guard.wrap(fake.executor())) == AGENT_OK, "crash tool");
    // No finish: process death after effects but before completed run receipt.
  }
  db.close(); check(db.open(path, &error), error);
  {
    const auto calls = fake.calls;
    WorkflowModelEffect guard(context("crash"), request);
    auto out = guard.preflight();
    check(out["blocker"]["kind"] == "model_run_outcome_unknown" && out["retryable"] == false && fake.calls == calls,
      "admitted model tool repeated after reopen");
  }
  {
    WorkflowModelEffect guard(context("provider"), request);
    check(guard.preflight().isNull(), "provider preflight");
    check(invoke(guard.wrap(fake.executor())) == AGENT_OK, "provider tool");
    Json::Value out; out["ok"] = false; out["error"] = "provider lost"; out["retryable"] = true;
    out = guard.finish(out);
    check(out["needs_input"] == true && out["retryable"] == false && out["effect"]["state"] == "unknown" &&
      out["run_error"] == "provider lost" && out["tool_calls_total"] == 1, "post-effect provider failure retried");
  }
  for (const char* id : {"invalid", "large", "throw", "timeout", "negative", "status"}) {
    Fake bad;
    const std::string kind = id;
    if (kind == "invalid") bad.result = "unstructured";
    if (kind == "large") bad.result = std::string(2 * 1024 * 1024 + 1, 'x');
    if (kind == "throw") bad.throws = true;
    if (kind == "timeout") bad.result = R"({"ok":false,"data":{"timed_out":true}})";
    if (kind == "negative") bad.result = R"({"ok":false,"data":{"exit_code":-1}})";
    if (kind == "status") bad.status = AGENT_ERR_INTERNAL;
    WorkflowModelEffect guard(context(id), request);
    check(guard.preflight().isNull(), kind + " preflight");
    auto executor = guard.wrap(bad.executor());
    check(invoke(executor) != AGENT_OK && invoke(executor) != AGENT_OK && bad.calls == 1, "uncertain tool allowed a later dispatch: " + kind);
    auto out = guard.finish(success()); // Even a swallowed core error cannot certify completion.
    check(out["ok"] == false && out["needs_input"] == true && out["effect"]["state"] == "unknown" &&
      out["tool_calls_total"] == 1, "invalid receipt became success: " + kind);
  }
  {
    auto ctx = context("capacity"); ctx.dispatch_capacity_available = [] { return false; };
    WorkflowModelEffect guard(ctx, request); check(guard.preflight().isNull(), "capacity preflight");
    const auto calls = fake.calls;
    check(invoke(guard.wrap(fake.executor())) != AGENT_OK && fake.calls == calls, "capacity bypass");
    check(guard.finish(success())["needs_input"] != true, "capacity denial fabricated an effect");
  }
  {
    bool stop = true; auto ctx = context("stop"); ctx.should_cancel = [&] { return stop; };
    WorkflowModelEffect early(ctx, request); check(early.preflight()["cancelled"] == true, "owner stop ignored");
    stop = false;
    WorkflowModelEffect guard(ctx, request); check(guard.preflight().isNull(), "stop preflight");
    check(invoke(guard.wrap(fake.executor())) == AGENT_OK, "stop tool");
    stop = true;
    const auto calls = fake.calls;
    check(invoke(guard.wrap(fake.executor())) != AGENT_OK && fake.calls == calls, "stop dispatched another tool");
    auto out = guard.finish(success());
    check(out["cancelled"] == true && out["retryable"] == false && out["effect"]["state"] == "unknown", "stop revived work");
  }
  {
    WorkflowModelEffect first(context("race"), request), second(context("race"), request);
    check(first.preflight().isNull() && second.preflight().isNull(), "race preflight");
    check(invoke(first.wrap(fake.executor())) == AGENT_OK, "first race dispatch");
    const auto calls = fake.calls;
    check(invoke(second.wrap(fake.executor())) != AGENT_OK && fake.calls == calls, "stale runner dispatched");
    check(second.finish(success())["needs_input"] == true, "stale runner was not fenced");
    check(first.finish(success())["ok"] == true, "winner receipt lost");
  }
  {
    sql("CREATE TRIGGER deny_dispatch BEFORE UPDATE OF state ON workflow_effects WHEN NEW.task_id='admission' AND NEW.state='dispatching' BEGIN SELECT RAISE(FAIL,'fixture deny'); END");
    WorkflowModelEffect guard(context("admission"), request); check(guard.preflight().isNull(), "admission preflight");
    const auto calls = fake.calls;
    check(invoke(guard.wrap(fake.executor())) != AGENT_OK && fake.calls == calls, "uncommitted admission executed");
    check(guard.finish(success())["retryable"] == true, "known no-dispatch retry was forbidden");
    sql("DROP TRIGGER deny_dispatch");
    WorkflowModelEffect next(context("admission"), request); check(next.preflight().isNull(), "prepared cannot recover");
    check(invoke(next.wrap(fake.executor())) == AGENT_OK && next.finish(success())["ok"] == true, "prepared recovery failed");
  }
  {
    sql("CREATE TRIGGER deny_receipt BEFORE UPDATE OF state ON workflow_effects WHEN NEW.task_id='receipt' AND NEW.state='completed' BEGIN SELECT RAISE(FAIL,'fixture receipt'); END");
    WorkflowModelEffect guard(context("receipt"), request); check(guard.preflight().isNull(), "receipt preflight");
    check(invoke(guard.wrap(fake.executor())) == AGENT_OK, "receipt tool");
    auto out = guard.finish(success());
    check(out["needs_input"] == true && out["effect"]["state"] == "dispatching", "lost persistence reported success");
    sql("DROP TRIGGER deny_receipt");
    WorkflowModelEffect next(context("receipt"), request);
    check(next.preflight()["needs_input"] == true, "lost receipt admitted a retry");
  }
  {
    Fake failed; failed.result = R"({"ok":false,"data":{"exit_code":4}})";
    WorkflowModelEffect guard(context("nonzero"), request); check(guard.preflight().isNull(), "nonzero preflight");
    check(invoke(guard.wrap(failed.executor())) == AGENT_OK, "known failed tool result should remain available to model");
    check(guard.finish(success())["ok"] == true, "model could not finish after handling a known tool failure");
  }
  db.close(); std::filesystem::remove_all(root);
  std::cout << "Model host boundary: lazy intent, receipts, crash, failure, CAS, budgets, stops and malformed results passed\n";
}
