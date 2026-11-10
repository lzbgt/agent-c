#include "workflow_effect.h"
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>

namespace {
void check(bool condition,const std::string& message) { if (!condition) { std::cerr << message << '\n'; std::exit(1); } }
Json::Value successful() {
  Json::Value out;
  out["ok"] = true; out["assistant_text"] = "receipt";
  out["http"]["status"] = 200; out["http"]["transport_ok"] = true;
  out["http"]["response_json"]["order"] = 42;
  return out;
}
}
int main() {
  using namespace agentd;
  const auto root = std::filesystem::temp_directory_path() / ("agent-effect-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  AgentDb db; std::string error;
  check(db.open((root / "state.sqlite").string(),&error),error);
  AgentDb::WorkflowRow workflow;
  workflow.workflow_id = "wf"; workflow.status = "queued"; workflow.spec_json = "{}";
  workflow.created_unix_ms = workflow.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const auto& id : {"once","crash","idempotent","reconcile","absent","cas","pending","nonfinal","wrong_hash","http_error","cancel_midway","capacity"}) {
    AgentDb::WorkflowTaskRow task; task.workflow_id = "wf"; task.task_id = id;
    task.status = "queued"; task.request_json = "{}"; task.depends_on_json = "[]";
    task.created_unix_ms = task.updated_unix_ms = 1; tasks.push_back(task);
  }
  check(db.create_workflow(workflow,tasks,&error),error);
  // Upgrade an existing v33 task database without changing its workflow rows.
  db.close();
  sqlite3* old_db = nullptr;
  check(sqlite3_open((root / "state.sqlite").c_str(),&old_db) == SQLITE_OK,"cannot open migration fixture");
  check(sqlite3_exec(old_db,"DROP TABLE workflow_effects; UPDATE meta SET value='33' WHERE key='schema_version';",nullptr,nullptr,nullptr) == SQLITE_OK,"cannot create v33 fixture");
  sqlite3_close(old_db);
  check(db.open((root / "state.sqlite").string(),&error),error);
  std::vector<AgentDb::WorkflowTaskRow> migrated;
  check(db.list_workflow_tasks("wf",&migrated,&error) && migrated.size() == tasks.size(),"schema upgrade lost existing tasks");
  Json::Value spec;
  spec["url"] = "https://service.example/orders"; spec["method"] = "POST";
  spec["body"]["item"] = 42;
  int dispatches = 0;
  auto send = [&](const std::map<std::string,std::string>& h) {
    ++dispatches;
    check(h.at("Idempotency-Key").rfind("fx_",0) == 0,"missing stable effect header");
    return successful();
  };
  auto no_reconcile = [](const Json::Value&) { return Json::Value(); };
  WorkflowEffectContext context{&db,"wf","once"};
  auto out = workflow_effect_execute(context,spec,send,no_reconcile);
  check(out["ok"] == true && out["effect"]["state"] == "completed" && dispatches == 1,"initial effect failed");
  out = workflow_effect_execute(context,spec,send,no_reconcile);
  check(out["ok"] == true && out["effect"]["replayed"] == true && dispatches == 1,"settled receipt must prevent redispatch");
  auto changed = spec; changed["body"]["item"] = 43;
  out = workflow_effect_execute(context,changed,send,no_reconcile);
  check(out["ok"] == false && dispatches == 1,"changed payload must not reuse the same effect");
  context.task_id = "capacity";
  context.dispatch_capacity_available = [] { return false; };
  out = workflow_effect_execute(context,spec,send,no_reconcile);
  check(out["ok"] == false && dispatches == 1 && out["needs_input"] != true,"capacity refusal dispatched or fabricated uncertainty");
  context.dispatch_capacity_available = {};
  // Simulate process death immediately after dispatch admission.
  context.task_id = "crash";
  try { workflow_effect_execute(context,spec,[](const auto&) -> Json::Value { throw std::runtime_error("process death"); },no_reconcile); }
  catch (const std::runtime_error&) {}
  db.close(); check(db.open((root / "state.sqlite").string(),&error),error);
  out = workflow_effect_execute(context,spec,send,no_reconcile);
  check(out["needs_input"] == true && out["retryable"] == false && dispatches == 1,"unknown guarded outcome must not repeat a write");
  // A remote that honors idempotency can safely receive the same key again.
  context.task_id = "idempotent";
  auto idem = spec; idem["effect"]["mode"] = "idempotent";
  std::string key;
  out = workflow_effect_execute(context,idem,[&](const auto& h) {
    key = h.at("Idempotency-Key"); Json::Value lost; lost["ok"] = false; lost["retryable"] = true; lost["http"]["transport_ok"] = false; return lost;
  },no_reconcile);
  out = workflow_effect_execute(context,idem,[&](const auto& h) { check(h.at("Idempotency-Key") == key && h.at("X-Agent-Effect-Attempt") == "2","retry identity changed"); return successful(); },no_reconcile);
  check(out["ok"] == true,"idempotent recovery failed");
  // Reconcile a committed request after losing its response.
  auto reconciled = spec; reconciled["effect"]["reconcile_url"] = "https://service.example/receipts";
  for (const auto& task_id : {"reconcile","absent","pending","nonfinal","wrong_hash"}) {
    context.task_id = task_id;
    std::map<std::string,std::string> identity;
    try { workflow_effect_execute(context,reconciled,[&](const auto& h) -> Json::Value { identity=h; throw std::runtime_error("process death"); },no_reconcile); }
    catch (const std::runtime_error&) {}
    const auto before = dispatches;
    out = workflow_effect_execute(context,reconciled,send,[&](const Json::Value& query) {
      check(query["method"] == "GET" && query["url"].asString().find(identity.at("Idempotency-Key")) != std::string::npos,"invalid receipt query");
      Json::Value result; result["ok"] = true;
      auto& proof = result["http"]["response_json"];
      proof["schema"] = "agentd.effect.receipt.v1";
      proof["effect_id"] = identity.at("Idempotency-Key"); proof["request_hash"] = identity.at("X-Agent-Effect-Hash");
      proof["attempt"] = 1; proof["final"] = true;
      proof["state"] = std::string(task_id) == "reconcile" ? "committed" : std::string(task_id) == "pending" ? "pending" : "not_applied";
      if (std::string(task_id) == "nonfinal") proof["final"] = false;
      if (std::string(task_id) == "wrong_hash") proof["request_hash"] = "wrong";
      proof["result"]["status"] = 200; proof["result"]["response_json"]["order"] = 42;
      return result;
    });
    if (std::string(task_id) == "pending") check(out["retryable"] == true && dispatches == before,"pending reconciliation must only poll");
    else if (std::string(task_id) == "nonfinal" || std::string(task_id) == "wrong_hash") check(out["needs_input"] == true && dispatches == before,"absence without matching terminal closure allowed a write");
    else check(out["ok"] == true && dispatches == before + (std::string(task_id) == "absent" ? 1 : 0),"reconciliation dispatched an unsafe duplicate");
  }
  context.task_id = "http_error";
  int failures = 0;
  auto server_error = [&](const auto&) { ++failures; auto result = successful(); result["ok"] = false; result["http"]["status"] = 500; result["retryable"] = true; return result; };
  out = workflow_effect_execute(context,spec,server_error,no_reconcile);
  out = workflow_effect_execute(context,spec,server_error,no_reconcile);
  check(out["ok"] == false && out["retryable"] == false && failures == 1,"guarded 5xx must not cause a repeated write");
  context.task_id = "cancel_midway";
  std::map<std::string,std::string> mid_identity;
  try { workflow_effect_execute(context,reconciled,[&](const auto& headers) -> Json::Value { mid_identity=headers; throw std::runtime_error("process death"); },no_reconcile); }
  catch (const std::runtime_error&) {}
  bool stop_requested = false;
  context.should_cancel = [&]() { return stop_requested; };
  const auto before_cancel = dispatches;
  out = workflow_effect_execute(context,reconciled,send,[&](const auto&) {
    stop_requested = true;
    Json::Value result; result["ok"] = true;
    auto& proof = result["http"]["response_json"];
    proof["schema"] = "agentd.effect.receipt.v1"; proof["effect_id"] = mid_identity.at("Idempotency-Key");
    proof["request_hash"] = mid_identity.at("X-Agent-Effect-Hash"); proof["attempt"] = 1;
    proof["final"] = true; proof["state"] = "not_applied";
    return result;
  });
  check(out["cancelled"] == true && dispatches == before_cancel,"cancellation during lookup admitted a new write");
  // Compare-and-swap prevents an old attempt from settling a newer dispatch.
  AgentDb::WorkflowEffectRow def, prepared, claimed, next;
  def.effect_id = "cas"; def.workflow_id = "wf"; def.task_id = "cas"; def.request_hash = "hash"; def.mode = "idempotent";
  check(db.prepare_workflow_effect(def,&prepared,&error),error);
  check(db.transition_workflow_effect(prepared,"dispatching","",&claimed,&error),error);
  check(!db.transition_workflow_effect(prepared,"dispatching","",&next,&error),"concurrent stale claim accepted");
  check(db.transition_workflow_effect(claimed,"dispatching","",&next,&error),error);
  check(!db.transition_workflow_effect(claimed,"completed","{}",&prepared,&error),"stale receipt accepted");
  auto invalid = spec; invalid["effect"]["mode"] = "unsafe";
  check(!workflow_effect_validate_http(invalid,&error),"unknown mode accepted");
  invalid = spec; invalid["effect"]["reconcile_url"] = "https://other.example/receipts";
  check(!workflow_effect_validate_http(invalid,&error),"cross-origin credential forwarding accepted");
  invalid = spec; invalid["headers"]["idempotency-key"] = "caller-key";
  check(!workflow_effect_validate_http(invalid,&error),"caller may override effect identity");
  invalid = spec; invalid["method"] = " post "; invalid["headers"][" Idempotency-Key "] = "caller-key";
  check(!workflow_effect_validate_http(invalid,&error),"normalization allowed a caller effect identity");
  // An old finalizer must not overwrite a requeued requirement or owner stop.
  AgentDb::WorkflowTaskRow failed = tasks[1];
  failed.status = "error"; failed.attempt = 1; failed.error = "unknown outcome";
  check(db.upsert_workflow_task(failed,&error),error);
  AgentDb::WorkflowRow before;
  std::vector<AgentDb::WorkflowTaskRow> snapshot;
  std::vector<AgentDb::WorkflowEffectRow> effects;
  check(db.get_workflow("wf",&before,&error) && db.list_workflow_tasks("wf",&snapshot,&error) && db.list_workflow_effects("wf",&effects,&error),error);
  const auto uncertain = std::find_if(effects.begin(),effects.end(),[](const auto& row) { return row.task_id == "crash"; });
  check(uncertain != effects.end(),"uncertain effect missing");
  check(db.requeue_workflow_effect(failed,*uncertain,"https://service.example/receipts",&error),error);
  auto final = before; final.status = "error";
  check(!db.update_workflow_from_task_snapshot(before,snapshot,final,&error),"stale finalizer overwrote a resumed task");
  check(db.get_workflow("wf",&before,&error) && db.list_workflow_tasks("wf",&snapshot,&error),error);
  auto cancelled = before; cancelled.cancel_requested = true;
  check(db.upsert_workflow(cancelled,&error),error);
  check(!db.update_workflow_from_task_snapshot(before,snapshot,final,&error),"stale finalizer overwrote an owner stop");
  check(db.upsert_workflow(before,&error) && db.get_workflow("wf",&cancelled,&error) && cancelled.cancel_requested,"stale worker cleared an owner stop");
  db.close(); std::filesystem::remove_all(root);
}
