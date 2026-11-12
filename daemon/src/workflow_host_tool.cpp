#include "workflow_host_tool.h"

#include "approval_queue.h"
#include "daemon_config.h"
#include "json_util.h"
#include "policy_hooks.h"
#include "string_util.h"
#include "toolset_host.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace agentd {
namespace {
std::string encode(const Json::Value& v) {
  Json::StreamWriterBuilder writer; writer["indentation"]="";
  return Json::writeString(writer,v);
}
Json::Value failure(const std::string& error) {
  Json::Value out(Json::objectValue);
  out["ok"]=false; out["error"]=error; out["retryable"]=false;
  out["assistant_text"]=""; out["tool_calls_total"]=Json::Int64(0);
  out["steps_executed"]=Json::Int64(0);
  return out;
}
struct HostTools {
  agent_tool_registry_t* registry=nullptr;
  agent_tool_executor_t executor{};
  ~HostTools() { if (registry) agent_tool_registry_destroy(registry); toolset_host_destroy(&executor); }
};
}

bool workflow_host_tool_validate(const Json::Value& spec, std::string* error) {
  if (error) error->clear();
  const auto fail=[&](const std::string& why) { if (error) *error=why; return false; };
  if (!spec.isObject()) return fail("host_tool must be an object");
  for (const auto& field : spec.getMemberNames())
    if (field!="name" && field!="arguments" && field!="timeout_ms") return fail("unknown host_tool field: "+field);
  if (!spec["name"].isString() || spec["name"].asString().empty() || spec["name"].asString().size()>128)
    return fail("host_tool.name must be a nonempty tool name");
  for (const char c : spec["name"].asString())
    if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-'))
      return fail("host_tool.name contains invalid characters");
  if (!spec["arguments"].isObject() || encode(spec["arguments"]).size()>256*1024)
    return fail("host_tool.arguments must be an object of at most 256 KiB");
  if (spec.isMember("timeout_ms") && (!spec["timeout_ms"].isInt64() ||
      spec["timeout_ms"].asInt64()<1 || spec["timeout_ms"].asInt64()>300000))
    return fail("host_tool.timeout_ms must be between 1 and 300000");
  return true;
}

Json::Value workflow_host_tool_execute(const DaemonConfig& cfg,
    const WorkflowEffectContext& context, const Json::Value& spec,
    const std::string& sessions_root, const std::string& session_id, int64_t elapsed_grant_ms) {
  std::string error;
  if (!workflow_host_tool_validate(spec,&error)) return failure(error);
  if (lower_copy(trim_copy(cfg.tools))!="host") return failure("host_tool requires --tools host");
  if (!context.db || !context.db->is_open()) return failure("host_tool requires durable database");
  const auto name=spec["name"].asString();
  const auto arguments=encode(spec["arguments"]);
  const auto policy=policy_config_from_daemon(cfg);
  if (policy.mode==PolicyMode::Enforce && policy.max_tool_call_args_chars>0 &&
      arguments.size()>policy.max_tool_call_args_chars) return failure("host_tool arguments exceed policy limit");
  const int64_t timeout=spec.get("timeout_ms",Json::Int64(30000)).asInt64();
  const int64_t admitted_timeout=elapsed_grant_ms>0 ? std::min(timeout,elapsed_grant_ms) : timeout;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(admitted_timeout);
  std::function<bool()> cancelled=[&]() {
    return (context.should_cancel && context.should_cancel()) || std::chrono::steady_clock::now()>=deadline;
  };
  if (cancelled()) return failure("host_tool cancelled before dispatch");

  HostToolsetConfig host;
  // This deterministic boundary is rooted in daemon state, never a caller-supplied broader scope.
  if (!cfg.yolo_default && cfg.state_dir.empty()) return failure("scoped host_tool requires daemon state_dir");
  std::error_code ec;
  const auto root=std::filesystem::absolute(cfg.yolo_default ? std::filesystem::current_path() :
      std::filesystem::path(cfg.state_dir),ec).lexically_normal();
  if (ec) return failure("cannot resolve host-tool execution root");
  host.root_dir=cfg.yolo_default ? "" : root.string();
  host.policy=cfg.host_policy; host.enable_process_exec=cfg.yolo_default; host.allow_symlinks=cfg.yolo_default;
  // The legacy host adapter treats a session as unrestricted. Never attach that
  // context to this scoped boundary; file APIs remain contained in state_dir.
  if (cfg.yolo_default) { host.sessions_root_dir=sessions_root; host.session_id=session_id; }
  host.should_cancel=[](void* ctx) { return (*static_cast<std::function<bool()>*>(ctx))(); };
  host.should_cancel_ctx=&cancelled;
  HostTools tools;
  if (toolset_host_create(host,&tools.registry,&tools.executor)!=AGENT_OK || !tools.registry || !tools.executor.execute)
    return failure("cannot initialize host toolset");
  bool registered=false;
  for (size_t i=0;i<agent_tool_registry_count(tools.registry);++i) {
    agent_tool_def_view_t tool{};
    if (agent_tool_registry_get(tools.registry,i,&tool)==AGENT_OK && tool.name && name==tool.name) registered=true;
  }
  if (!registered) return failure("host_tool is unavailable under daemon host policy: "+name);

  PolicyHookCtx hook;
  policy_prepare(&hook,policy,context.workflow_id+":"+context.task_id,"",session_id);
  hook.last_tool_call_id=context.workflow_id+":"+context.task_id;
  ApprovalGateCtx approval;
  approval.db=context.db; approval.hook=&hook; approval.trace_id=hook.trace_id;
  approval.session_id=session_id; approval.enforce=policy.mode==PolicyMode::Enforce;
  approval.audit=policy.mode==PolicyMode::Audit; approval.poll_ms=std::max<int64_t>(1,policy.approval_poll_ms);
  approval.should_cancel=cancelled;
  if (!policy.approval_rules.empty()) {
    for (const auto& rule : policy.approval_rules) for (const auto& tool : rule.tool_names) {
      const auto key=trim_copy(tool);
      if (key.empty()) continue;
      approval.tool_rules[key]={std::max(0,rule.required),rule.roles,std::max<int64_t>(0,rule.timeout_ms),
                               rule.require_distinct_roles,rule.best_effort};
    }
  } else {
    for (const auto& tool : policy.approval_tools) if (!tool.empty()) approval.toolset.insert(tool);
    approval.required=std::max(0,policy.approval_required); approval.roles=policy.approval_roles;
    approval.timeout_ms=std::max<int64_t>(0,policy.approval_timeout_ms);
  }
  // Run the existing policy/approval gate before any dispatch admission, not a second execution path.
  PolicyToolExecutorCtx gate;
  gate.hook=&hook; gate.approval_gate=&approval;
  gate.base.execute=[](void*,const char*,const char*,agent_string_t*) { return AGENT_OK; };
  const auto authorize=[&]() {
    agent_string_t unused{};
    const auto allowed=policy_tool_execute(&gate,name.c_str(),arguments.c_str(),&unused);
    agent_string_free(&unused);
    if (allowed!=AGENT_OK) return failure(hook.last_error.empty() ? "host_tool blocked by policy" : hook.last_error);
    if (cancelled()) return failure("host_tool cancelled before dispatch");
    return Json::Value();
  };

  auto guarded=context; guarded.should_cancel=cancelled;
  Json::Value request;
  request["protocol"]="agentd.host_tool.v1"; request["tool"]=spec;
  request["root"]=root.string(); request["yolo"]=cfg.yolo_default;
  request["sessions_root"]=sessions_root; request["session_id"]=session_id;
  int64_t calls=0;
  auto out=workflow_effect_execute_guarded(guarded,request,[&]() -> GuardedEffectResult {
    ++calls;
    auto execution_args=spec["arguments"];
    if (name=="shell_exec" || name=="proc_exec") {
      const auto configured=execution_args.get("timeout_ms",Json::Int64(admitted_timeout));
      const auto limit=configured.isInt64() && configured.asInt64()>0 ?
        std::min<int64_t>(configured.asInt64(),admitted_timeout) : admitted_timeout;
      execution_args["timeout_ms"]=Json::Int64(limit);
      const auto maximum=execution_args.get("max_output_bytes",Json::Int64(65536));
      execution_args["max_output_bytes"]=Json::Int64(maximum.isInt64() ?
        std::clamp<int64_t>(maximum.asInt64(),1,1024*1024) : 65536);
    }
    agent_string_t response{};
    const auto serialized=encode(execution_args);
    const auto status=tools.executor.execute(tools.executor.ctx,name.c_str(),serialized.c_str(),&response);
    const size_t maximum=policy.mode==PolicyMode::Enforce && policy.max_tool_result_chars>0 ?
      std::min<size_t>(policy.max_tool_result_chars,2*1024*1024) : 2*1024*1024;
    const bool bounded=response.len<=maximum;
    const std::string raw=bounded && response.data ? std::string(response.data,response.len) : "";
    agent_string_free(&response);
    Json::Value parsed;
    std::string parse_error;
    if (status!=AGENT_OK || !bounded || !json_parse_any(raw,&parsed,&parse_error) ||
        !parsed.isObject() || !parsed["ok"].isBool())
      return {failure("host tool did not return a bounded valid result"),false};
    Json::Value result;
    result["ok"]=parsed["ok"]; result["assistant_text"]="";
    result["host_tool"]["name"]=name; result["host_tool"]["result"]=parsed;
    const auto& data=parsed["data"];
    if (data.isObject() && data["output"].isString()) result["assistant_text"]=data["output"];
    if (parsed["error"].isString()) result["error"]=parsed["error"];
    const bool interrupted=parsed["cancelled"]==true || (data.isObject() &&
      (data["cancelled"]==true || data["timed_out"]==true ||
       ((name=="shell_exec" || name=="proc_exec") && data["exit_code"].isInt() && data["exit_code"].asInt()<0)));
    if (interrupted) result["error"]="host tool timed out or was interrupted; downstream effects are unverified";
    return {result,!interrupted};
  },authorize);
  out["kind"]="host_tool";
  out["tool_calls_total"]=Json::Int64(calls);
  out["steps_executed"]=Json::Int64(1);
  return out;
}
}  // namespace agentd
