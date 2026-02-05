#include "workflow_memory_ops.h"

#include "memory_consolidator.h"
#include "string_util.h"
#include "toolset_host.h"

namespace agentd {
namespace workflow_engine_internal {
namespace {

static void maybe_emit_checkpoint_event(
  AgentDb* db,
  const std::string& workflow_id,
  const std::string& task_id,
  const std::string& trace_id_opt,
  const Json::Value& memory_put_response,
  int64_t now_unix_ms
) {
  if (!db) return;
  if (workflow_id.empty() || task_id.empty()) return;
  if (!memory_put_response.isObject()) return;
  if (!memory_put_response.isMember("data") || !memory_put_response["data"].isObject()) return;

  const Json::Value d = memory_put_response["data"];
  const bool ck_ok = d.isMember("checkpoint_ok") && d["checkpoint_ok"].isBool() && d["checkpoint_ok"].asBool();
  const std::string ck_path =
    d.isMember("checkpoint_path") && d["checkpoint_path"].isString() ? d["checkpoint_path"].asString() : "";
  const std::string ck_sha =
    d.isMember("checkpoint_sha256") && d["checkpoint_sha256"].isString() ? d["checkpoint_sha256"].asString() : "";
  const std::string ck_ts =
    d.isMember("checkpoint_ts_utc") && d["checkpoint_ts_utc"].isString() ? d["checkpoint_ts_utc"].asString() : "";
  const int64_t ck_bytes =
    d.isMember("checkpoint_bytes") && (d["checkpoint_bytes"].isInt64() || d["checkpoint_bytes"].isUInt64() || d["checkpoint_bytes"].isInt())
    ? d["checkpoint_bytes"].asInt64()
    : 0;

  if (!ck_ok || ck_path.empty() || ck_sha.empty()) return;

  Json::Value ck(Json::objectValue);
  ck["path"] = ck_path;
  ck["sha256"] = ck_sha;
  if (!ck_ts.empty()) ck["ts_utc"] = ck_ts;
  if (ck_bytes > 0) ck["bytes"] = (Json::Int64)ck_bytes;

  Json::Value ev(Json::objectValue);
  ev["workflow_id"] = workflow_id;
  ev["task_id"] = task_id;
  if (!trace_id_opt.empty()) ev["trace_id"] = trace_id_opt;
  ev["checkpoint"] = ck;
  ev["ts_unix_ms"] = (Json::Int64)now_unix_ms;
  insert_workflow_event_best_effort(db, workflow_id, task_id, "memory_checkpoint", now_unix_ms, ev);
}

static void maybe_attach_checkpoint_surface(Json::Value* out, const Json::Value& memory_put_response) {
  if (!out || !out->isObject()) return;
  if (!memory_put_response.isObject()) return;
  if (!memory_put_response.isMember("data") || !memory_put_response["data"].isObject()) return;

  const Json::Value d = memory_put_response["data"];
  const bool ck_ok = d.isMember("checkpoint_ok") && d["checkpoint_ok"].isBool() && d["checkpoint_ok"].asBool();
  const std::string ck_path =
    d.isMember("checkpoint_path") && d["checkpoint_path"].isString() ? d["checkpoint_path"].asString() : "";
  const std::string ck_sha =
    d.isMember("checkpoint_sha256") && d["checkpoint_sha256"].isString() ? d["checkpoint_sha256"].asString() : "";
  const std::string ck_ts =
    d.isMember("checkpoint_ts_utc") && d["checkpoint_ts_utc"].isString() ? d["checkpoint_ts_utc"].asString() : "";
  const int64_t ck_bytes =
    d.isMember("checkpoint_bytes") && (d["checkpoint_bytes"].isInt64() || d["checkpoint_bytes"].isUInt64() || d["checkpoint_bytes"].isInt())
    ? d["checkpoint_bytes"].asInt64()
    : 0;

  if (!ck_ok || ck_path.empty() || ck_sha.empty()) return;

  Json::Value ck(Json::objectValue);
  ck["path"] = ck_path;
  ck["sha256"] = ck_sha;
  if (!ck_ts.empty()) ck["ts_utc"] = ck_ts;
  if (ck_bytes > 0) ck["bytes"] = (Json::Int64)ck_bytes;
  (*out)["checkpoint"] = ck;
}

}  // namespace

Json::Value workflow_memory_put_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const std::string& sessions_root_dir_fallback,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  WorkflowRunCancelCtx* cancel_ctx_or_null,
  int64_t now_unix_ms
) {
  Json::Value out(Json::objectValue);
  out["kind"] = "memory_put";
  out["ok"] = false;
  out["assistant_text"] = "";

  const std::string ttrace = (rr.isObject() && rr.isMember("trace_id") && rr["trace_id"].isString()) ? trim_copy(rr["trace_id"].asString()) : "";

  if (cancel_ctx_or_null && workflow_run_should_cancel(cancel_ctx_or_null)) {
    out["cancelled"] = true;
    out["error"] = cancel_ctx_or_null->reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    return out;
  }
  if (lower_copy(trim_copy(cfg.tools)) != "host") {
    out["error"] = "memory_put requires --tools host";
    return out;
  }
  if (cfg.host_policy != HostToolsetPolicyMode::Full) {
    out["error"] = "memory_put requires host_policy=full";
    return out;
  }

  const Json::Value mp =
    rr.isMember("memory_put") && rr["memory_put"].isObject() ? rr["memory_put"] : Json::Value(Json::nullValue);
  if (!mp.isObject()) {
    out["error"] = "memory_put missing memory_put object";
    return out;
  }

  const std::string path =
    mp.isMember("path") && mp["path"].isString() ? trim_copy(mp["path"].asString()) : "STRUCTURED.md";
  const Json::Value entries = mp.isMember("entries") ? mp["entries"] : Json::Value(Json::nullValue);
  if (!entries.isArray() || entries.empty()) {
    out["error"] = "memory_put.entries must be a non-empty array";
    return out;
  }

  std::string corr = std::string("workflow:") + wf.workflow_id + " task:" + task.task_id;
  if (!wf.session_id.empty()) corr += std::string(" session:") + wf.session_id;
  if (!ttrace.empty()) corr += std::string(" trace:") + ttrace;

  Json::Value tool_entries(Json::arrayValue);
  int valid = 0;
  for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
    const Json::Value e = entries[i];
    if (!e.isObject()) continue;
    const std::string key = e.isMember("key") && e["key"].isString() ? trim_copy(e["key"].asString()) : "";
    const std::string value = e.isMember("value") && e["value"].isString() ? e["value"].asString() : "";
    if (key.empty() || value.empty()) continue;
    valid++;

    const std::string src =
      e.isMember("source") && e["source"].isString() ? trim_copy(e["source"].asString()) : "";
    if (src.empty()) {
      Json::Value e2 = e;
      e2["source"] = corr;
      tool_entries.append(e2);
    } else {
      tool_entries.append(e);
      if (!corr.empty() && src != corr) {
        Json::Value e3 = e;
        e3["source"] = corr;
        tool_entries.append(e3);
      }
    }
  }
  if (valid == 0) {
    out["error"] = "memory_put.entries has no valid entries (each entry requires key + value)";
    return out;
  }

  Json::Value args(Json::objectValue);
  args["path"] = path.empty() ? "STRUCTURED.md" : path;
  args["entries"] = tool_entries;
  const bool checkpoint =
    !mp.isMember("checkpoint") || (mp["checkpoint"].isBool() && mp["checkpoint"].asBool());
  args["checkpoint"] = checkpoint;
  int keep_checkpoints = cfg.memory_consolidate_keep_checkpoints > 0 ? cfg.memory_consolidate_keep_checkpoints : 100;
  if (mp.isMember("keep_checkpoints") && mp["keep_checkpoints"].isInt()) {
    keep_checkpoints = std::max(1, mp["keep_checkpoints"].asInt());
  }
  args["keep_checkpoints"] = keep_checkpoints;

  HostToolsetConfig hcfg;
  hcfg.root_dir = cfg.state_dir;
  hcfg.policy = HostToolsetPolicyMode::Full;
  hcfg.enable_process_exec = false;
  hcfg.allow_symlinks = true;
  hcfg.sessions_root_dir = !cfg.sessions_root_dir.empty() ? cfg.sessions_root_dir : sessions_root_dir_fallback;
  hcfg.session_id = wf.session_id;

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
  if (st != AGENT_OK || !reg || !exec.execute) {
    if (reg) agent_tool_registry_destroy(reg);
    toolset_host_destroy(&exec);
    out["error"] = "failed to create host toolset";
    return out;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string req = Json::writeString(wb, args);

  agent_string_t out_s{};
  // Budget charging: deterministic host-tool invocation => one tool call / step.
  const agent_status_t est = exec.execute(exec.ctx, "memory_put", req.c_str(), &out_s);
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  const std::string resp_s = (out_s.data && out_s.len) ? std::string(out_s.data, out_s.len) : std::string();
  agent_string_free(&out_s);

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);

  if (est != AGENT_OK) {
    out["error"] = "memory_put failed";
    return out;
  }

  Json::Value resp(Json::objectValue);
  std::string rerr;
  if (!json_parse_any_value(resp_s, &resp, &rerr) || !resp.isObject()) {
    out["error"] = "failed to parse memory_put response";
    out["parse_error"] = rerr;
    return out;
  }

  out["memory_put_response"] = resp;
  const bool ok = resp.isMember("ok") && resp["ok"].isBool() && resp["ok"].asBool();
  out["ok"] = ok;
  out["path"] = args["path"];
  if (!ok) {
    const std::string err = resp.isMember("error") && resp["error"].isString() ? resp["error"].asString() : "memory_put failed";
    out["error"] = err;
    out["assistant_text"] = "";
    return out;
  }

  maybe_attach_checkpoint_surface(&out, resp);
  maybe_emit_checkpoint_event(db, wf.workflow_id, task.task_id, ttrace, resp, now_unix_ms);

  const std::string output =
    resp.isMember("data") && resp["data"].isObject() && resp["data"].isMember("output") && resp["data"]["output"].isString()
    ? resp["data"]["output"].asString()
    : (std::string("memory_put: ") + args["path"].asString());
  out["assistant_text"] = output;
  return out;
}

Json::Value workflow_memory_consolidate_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  WorkflowRunCancelCtx* cancel_ctx_or_null,
  int64_t now_unix_ms
) {
  Json::Value out(Json::objectValue);
  out["kind"] = "memory_consolidate";
  out["ok"] = false;
  out["assistant_text"] = "";

  const std::string ttrace = (rr.isObject() && rr.isMember("trace_id") && rr["trace_id"].isString()) ? trim_copy(rr["trace_id"].asString()) : "";

  if (cancel_ctx_or_null && workflow_run_should_cancel(cancel_ctx_or_null)) {
    out["cancelled"] = true;
    out["error"] = cancel_ctx_or_null->reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    return out;
  }
  if (lower_copy(trim_copy(cfg.tools)) != "host") {
    out["error"] = "memory_consolidate requires --tools host";
    return out;
  }
  if (cfg.host_policy != HostToolsetPolicyMode::Full) {
    out["error"] = "memory_consolidate requires host_policy=full";
    return out;
  }

  const Json::Value mc =
    rr.isMember("memory_consolidate") && rr["memory_consolidate"].isObject() ? rr["memory_consolidate"] : Json::Value(Json::objectValue);

  MemoryConsolidateOptions opt;
  opt.daily_days = cfg.memory_consolidate_daily_days;
  opt.keep_checkpoints = cfg.memory_consolidate_keep_checkpoints;

  if (mc.isMember("daily_days") && mc["daily_days"].isInt()) {
    opt.daily_days = std::max(0, mc["daily_days"].asInt());
  }
  if (mc.isMember("keep_checkpoints") && mc["keep_checkpoints"].isInt()) {
    opt.keep_checkpoints = std::max(1, mc["keep_checkpoints"].asInt());
  }
  if (mc.isMember("max_entries") && mc["max_entries"].isInt()) {
    opt.max_entries = std::max(1, mc["max_entries"].asInt());
  }
  if (mc.isMember("dry_run") && mc["dry_run"].isBool()) {
    opt.dry_run = mc["dry_run"].asBool();
  }

  Json::Value report;
  std::string merr;
  // Budget charging: deterministic host-side job => one tool call / step.
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  if (!memory_consolidate_once(cfg, opt, &report, &merr)) {
    out["ok"] = false;
    out["error"] = merr.empty() ? "memory consolidation failed" : merr;
    return out;
  }

  out["ok"] = true;
  out["report"] = report;

  // Surface checkpoint evidence for correlation.
  if (report.isObject() && report.isMember("memory_put_response") && report["memory_put_response"].isObject()) {
    const Json::Value resp = report["memory_put_response"];
    maybe_attach_checkpoint_surface(&out, resp);
    maybe_emit_checkpoint_event(db, wf.workflow_id, task.task_id, ttrace, resp, now_unix_ms);
  }

  if (report.isObject() && report.isMember("output") && report["output"].isString()) {
    out["assistant_text"] = report["output"].asString();
  } else {
    out["assistant_text"] = "memory_consolidate: ok";
  }
  return out;
}

}  // namespace workflow_engine_internal
}  // namespace agentd

