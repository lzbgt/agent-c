#include "workflow_host_tool.h"
#include "daemon_config.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>

namespace {
void check(bool value,const std::string& reason) {
  if (!value) { std::cerr<<reason<<'\n'; std::exit(1); }
}
}
int main() {
  using namespace agentd;
  const auto root=std::filesystem::temp_directory_path() /
    ("agent-host-effect-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  AgentDb db; std::string error;
  check(db.open((root/"state.sqlite").string(),&error),error);
  AgentDb::WorkflowRow workflow;
  workflow.workflow_id="host"; workflow.status="queued"; workflow.spec_json="{}";
  workflow.created_unix_ms=workflow.updated_unix_ms=1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const auto* id : {"read","outside","deny","args","closed","unknown","invalid","throw","capacity","stop","malformed","approval"}) {
    AgentDb::WorkflowTaskRow task;
    task.workflow_id="host"; task.task_id=id; task.status="queued"; task.request_json="{}"; task.depends_on_json="[]";
    task.created_unix_ms=task.updated_unix_ms=1; tasks.push_back(task);
  }
  check(db.create_workflow(workflow,tasks,&error),error);
  { std::ofstream f(root/"receipt.txt"); f<<"original result"; }
  DaemonConfig cfg; cfg.tools="host"; cfg.yolo_default=false; cfg.state_dir=root.string();
  Json::Value spec; spec["name"]="fs_read"; spec["arguments"]["path"]="receipt.txt";
  WorkflowEffectContext context{&db,"host","read"};
  auto out=workflow_host_tool_execute(cfg,context,spec,"","session-does-not-expand-scope");
  check(out["ok"]==true && out["tool_calls_total"]==1 && out["effect"]["state"]=="completed",out.toStyledString());
  { std::ofstream f(root/"receipt.txt"); f<<"changed"; }
  out=workflow_host_tool_execute(cfg,context,spec,"","session-does-not-expand-scope");
  check(out["ok"]==true && out["effect"]["replayed"]==true && out["tool_calls_total"]==0, "cached host result was executed again");
  check(out.toStyledString().find("original result")!=std::string::npos,"original saved result lost");
  auto changed=spec; changed["arguments"]["path"]="different";
  out=workflow_host_tool_execute(cfg,context,changed,"","session-does-not-expand-scope");
  check(out["ok"]==false && out["tool_calls_total"]==0,"changed request reused original effect");
  context.task_id="outside"; changed["arguments"]["path"]="../outside";
  out=workflow_host_tool_execute(cfg,context,changed,"","session-does-not-expand-scope");
  check(out["ok"]==false,"session context escaped scoped host root");
  context.task_id="deny"; cfg.policy_mode="enforce"; cfg.policy_tool_denylist={"fs_read"};
  out=workflow_host_tool_execute(cfg,context,spec,"","");
  check(out["ok"]==false && out["tool_calls_total"]==0,"denylist bypassed");
  cfg.policy_tool_denylist.clear(); cfg.policy_max_tool_call_args_chars=1;
  context.task_id="args"; out=workflow_host_tool_execute(cfg,context,spec,"","");
  check(out["ok"]==false && out["tool_calls_total"]==0,"argument policy cap bypassed");
  cfg.policy_max_tool_call_args_chars=0;
  cfg.policy_approval_tools={"fs_read"};
  cfg.policy_approval_timeout_ms=100;
  context.task_id="read";
  out=workflow_host_tool_execute(cfg,context,spec,"","session-does-not-expand-scope");
  check(out["ok"]==true && out["effect"]["replayed"]==true && out["tool_calls_total"]==0,
        "saved receipt incorrectly requested a new approval");
  cfg.policy_approval_timeout_ms=5000;
  context.task_id="approval";
  int cancellation_checks=0;
  context.should_cancel=[&]() { return ++cancellation_checks>=4; };
  out=workflow_host_tool_execute(cfg,context,spec,"","");
  check(out["ok"]==false && out["tool_calls_total"]==0 && out["needs_input"]!=true,
        "approval cancellation dispatched or fabricated an unknown effect");
  cfg.policy_approval_tools.clear(); context.should_cancel={};
  auto bad=spec; bad["effect"]["mode"]="idempotent";
  check(!workflow_host_tool_validate(bad,&error),"unsafe host idempotency contract accepted");
  bad=spec; bad["timeout_ms"]=0;
  check(!workflow_host_tool_validate(bad,&error),"unbounded timeout accepted");

  int calls=0;
  Json::Value request; request["protocol"]="test"; request["name"]="write";
  auto completed=[&]() { ++calls; Json::Value result; result["ok"]=false; result["error"]="completed nonzero exit"; return GuardedEffectResult{result,true}; };
  context.task_id="closed";
  out=workflow_effect_execute_guarded(context,request,completed);
  out=workflow_effect_execute_guarded(context,request,completed);
  check(calls==1 && out["ok"]==false && out["retryable"]==false && out["effect"]["replayed"]==true,
        "known tool failure was automatically repeated");
  for (const auto* id : {"unknown","invalid","throw"}) {
    context.task_id=id;
    out=workflow_effect_execute_guarded(context,request,[&]() -> GuardedEffectResult {
      ++calls;
      if (context.task_id=="throw") throw std::runtime_error("lost process");
      Json::Value result;
      if (context.task_id=="unknown") { result["ok"]=false; result["error"]="timeout"; }
      return {result,false};
    });
    const auto before=calls;
    db.close(); check(db.open((root/"state.sqlite").string(),&error),error);
    out=workflow_effect_execute_guarded(context,request,completed);
    check(out["blocker"]["kind"]=="host_tool_outcome_unknown" && out["retryable"]==false && calls==before,
          "interrupted/malformed host result redispatched after restart");
  }
  context.task_id="capacity"; context.dispatch_capacity_available=[] { return false; };
  const auto before=calls;
  out=workflow_effect_execute_guarded(context,request,completed);
  check(out["ok"]==false && calls==before && out["needs_input"]!=true,"capacity refusal dispatched or fabricated uncertainty");
  context.dispatch_capacity_available={}; context.task_id="stop"; context.should_cancel=[] { return true; };
  out=workflow_effect_execute_guarded(context,request,completed);
  check(out["cancelled"]==true && calls==before,"owner stop dispatched");
  db.close(); std::filesystem::remove_all(root);
  std::cout<<"Host guarded identity, receipts, policy, scope, crash and capacity checks passed\n";
}
