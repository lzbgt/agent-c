#include "workflow_engine.h"

#include "avm_endpoints.h"
#include "edge_util.h"
#include "json_util.h"
#include "memory_consolidator.h"
#include "run_endpoints.h"
#include "string_util.h"
#include "toolset_host.h"
#include "workflow_aggregate.h"
#include "workflow_engine_common.h"
#include "workflow_fairq_cost.h"
#include "workflow_memory_correlate.h"
#include "workflow_memory_query.h"
#include "workflow_http_json.h"
#include "workflow_agentd_call.h"
#include "workflow_templates.h"
#include "workflow_memory_ops.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace agentd {
namespace wfi = workflow_engine_internal;
using namespace wfi;

WorkflowEngine::WorkflowEngine(
  AgentDb* db,
  std::function<DaemonConfig()> cfg_snapshot,
  std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg,
  const ToolExtension* tool_ext_or_null,
  std::string sessions_root_dir,
  Options opt
) : db_(db),
    cfg_snapshot_(std::move(cfg_snapshot)),
    ocfg_from_cfg_(std::move(ocfg_from_cfg)),
    tool_ext_or_null_(tool_ext_or_null),
    sessions_root_dir_(std::move(sessions_root_dir)),
    opt_(opt) {
  if (opt_.max_concurrency <= 0) opt_.max_concurrency = 1;
  if (opt_.max_concurrency > 16) opt_.max_concurrency = 16;
  if (opt_.poll_ms <= 0) opt_.poll_ms = 50;
  if (opt_.poll_ms > 5000) opt_.poll_ms = 5000;
  if (opt_.max_scan_workflows == 0) opt_.max_scan_workflows = 16;
  if (opt_.max_inflight_per_workflow <= 0) opt_.max_inflight_per_workflow = 1;
  if (opt_.max_inflight_per_workflow > 64) opt_.max_inflight_per_workflow = 64;
  if (opt_.max_inflight_per_session < 0) opt_.max_inflight_per_session = 0;
  if (opt_.max_inflight_per_session > 1024) opt_.max_inflight_per_session = 1024;

	  // Canonicalize fair-queue policy.
	  {
	    std::string p = opt_.fair_queue_policy;
	    for (char& c : p) {
	      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	    }
	    if (p != "scan_rr" && p != "wrr" && p != "drr") {
	      p = "wrr";
	    }
	    opt_.fair_queue_policy = p;
	  }
  if (opt_.fair_queue_max_session_weight <= 0) opt_.fair_queue_max_session_weight = 1;
  if (opt_.fair_queue_max_session_weight > 1024) opt_.fair_queue_max_session_weight = 1024;
  if (opt_.fair_queue_max_schedule_len < 16) opt_.fair_queue_max_schedule_len = 16;
  if (opt_.fair_queue_max_schedule_len > 65536) opt_.fair_queue_max_schedule_len = 65536;
}

WorkflowEngine::~WorkflowEngine() {
  stop();
}

std::string WorkflowEngine::json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

bool WorkflowEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_.load()) return true;
  if (!db_ || !db_->is_open()) {
    if (out_error) *out_error = "workflow engine requires an open AgentDb";
    return false;
  }
  if (!cfg_snapshot_ || !ocfg_from_cfg_) {
    if (out_error) *out_error = "workflow engine missing cfg snapshot function(s)";
    return false;
  }

  // Recovery (best-effort).
  {
    std::string err;
    (void)db_->recover_inflight_workflows(unix_ms_now(), &err);
  }

  stop_.store(false);
  running_.store(true);
  workers_.clear();
  workers_.reserve((size_t)opt_.max_concurrency);
  for (int i = 0; i < opt_.max_concurrency; i++) {
    workers_.emplace_back([this]() { worker_main(); });
  }
  return true;
}

void WorkflowEngine::stop() {
  stop_.store(true);
  if (!running_.load()) return;
  for (auto& th : workers_) {
    if (th.joinable()) th.join();
  }
  workers_.clear();
  running_.store(false);
}

void WorkflowEngine::worker_main() {
  while (!stop_.load()) {
    const int64_t now = unix_ms_now();
    AgentDb::WorkflowRow wf;
    AgentDb::WorkflowTaskRow task;
    std::string err;
    if (pick_and_claim_one(now, &wf, &task, &err)) {
      execute_claimed_task(wf, task);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
  }
}

void WorkflowEngine::execute_claimed_task(const AgentDb::WorkflowRow& wf, const AgentDb::WorkflowTaskRow& task) {
  const int64_t now = unix_ms_now();
  const auto started_steady = std::chrono::steady_clock::now();

  // Refresh workflow view (cancel_requested/deadline can change after claim).
  AgentDb::WorkflowRow wf_latest = wf;
  {
    AgentDb::WorkflowRow cur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &cur, &werr)) {
      wf_latest = std::move(cur);
    }
  }

  WorkflowRunCancelCtx cancel_ctx;
  cancel_ctx.db = db_;
  cancel_ctx.workflow_id = wf.workflow_id;
  cancel_ctx.deadline_unix_ms = workflow_deadline_unix_ms_best_effort(wf_latest);

  auto cancel_task_now = [&](const std::string& reason, const std::string& event_reason) {
    AgentDb::WorkflowTaskRow upd = task;
    upd.status = "cancelled";
    upd.updated_unix_ms = now;
    upd.finished_unix_ms = now;
    upd.ready_unix_ms = 0;
    upd.error = reason.empty() ? "cancelled" : reason;
    {
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["cancelled"] = true;
      out["assistant_text"] = "";
      out["error"] = upd.error;
      out["tool_calls_total"] = (Json::Int64)0;
      out["steps_executed"] = (Json::Int64)0;
      {
        const auto elapsed_ms =
          (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_steady).count();
        out["elapsed_ms"] = (Json::Int64)std::max<int64_t>(0, elapsed_ms);
      }
      upd.result_json = json_stringify_compact(out);
    }
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["reason"] = event_reason;
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    maybe_finalize_workflow(wf.workflow_id);
  };

  // If cancellation/deadline is already in effect, do not execute provider/tool calls.
  if (wf_latest.cancel_requested) {
    cancel_task_now("cancelled", "cancel_requested");
    return;
  }
  if (cancel_ctx.deadline_unix_ms > 0 && now > cancel_ctx.deadline_unix_ms) {
    // Ensure the workflow row reflects the deadline cancellation even if no other worker is scanning.
    AgentDb::WorkflowRow upd = wf_latest;
    upd.cancel_requested = true;
    upd.updated_unix_ms = now;
    if (upd.error.empty()) upd.error = "deadline exceeded";
    (void)db_->upsert_workflow(upd, nullptr);
    cancel_task_now("deadline exceeded", "deadline_exceeded");
    return;
  }

  // Resolve template vars from completed tasks (assistant_text only).
  std::unordered_map<std::string, std::string> assistant_by_task;
  std::unordered_map<std::string, Json::Value> result_json_by_task;
  int64_t workflow_tool_calls_used = 0;
  int64_t workflow_steps_used = 0;
  int64_t workflow_elapsed_ms_used = 0;
  int64_t workflow_prompt_tokens_used = 0;
  int64_t workflow_completion_tokens_used = 0;
  int64_t workflow_total_tokens_used = 0;
  auto saturating_add_i64 = [](int64_t a, int64_t b) -> int64_t {
    if (b <= 0) return a;
    if (a > (INT64_MAX - b)) return INT64_MAX;
    return a + b;
  };
  {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    std::string err;
    if (db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      for (const auto& t : tasks) {
        // Retry-safe budget accounting uses durable cumulative counters in workflow_tasks.
        workflow_tool_calls_used = saturating_add_i64(workflow_tool_calls_used, std::max<int64_t>(0, t.tool_calls_total_cum));
        workflow_steps_used = saturating_add_i64(workflow_steps_used, std::max<int64_t>(0, t.steps_executed_cum));
        workflow_elapsed_ms_used = saturating_add_i64(workflow_elapsed_ms_used, std::max<int64_t>(0, t.elapsed_ms_cum));
        workflow_prompt_tokens_used = saturating_add_i64(workflow_prompt_tokens_used, std::max<int64_t>(0, t.prompt_tokens_cum));
        workflow_completion_tokens_used = saturating_add_i64(workflow_completion_tokens_used, std::max<int64_t>(0, t.completion_tokens_cum));
        workflow_total_tokens_used = saturating_add_i64(workflow_total_tokens_used, std::max<int64_t>(0, t.total_tokens_cum));

        if (t.result_json.empty()) continue;
        Json::Value r;
        std::string perr;
        if (!json_parse_any_value(t.result_json, &r, &perr) || !r.isObject()) continue;

        if (t.task_id.empty()) continue;
        if (t.status != "done" && t.status != "error") continue;
        const std::string a = json_get_string(r, "assistant_text");
        if (!a.empty()) assistant_by_task[t.task_id] = a;
        result_json_by_task[t.task_id] = r;
      }
    }
  }

  const WorkflowLimits wf_limits = workflow_limits_best_effort(wf_latest);
  int64_t wf_tool_calls_remaining = 0;
  if (wf_limits.max_tool_calls_total > 0) {
    const int64_t max_total = wf_limits.max_tool_calls_total;
    const int64_t used = std::max<int64_t>(0, std::min<int64_t>(max_total, workflow_tool_calls_used));
    wf_tool_calls_remaining = std::max<int64_t>(0, max_total - used);
  }
  int64_t wf_steps_remaining = 0;
  if (wf_limits.max_steps_total > 0) {
    const int64_t max_total = wf_limits.max_steps_total;
    const int64_t used = std::max<int64_t>(0, std::min<int64_t>(max_total, workflow_steps_used));
    wf_steps_remaining = std::max<int64_t>(0, max_total - used);
  }
  int64_t wf_elapsed_ms_remaining = 0;
  if (wf_limits.max_elapsed_ms_total > 0) {
    const int64_t max_total = wf_limits.max_elapsed_ms_total;
    const int64_t used = std::max<int64_t>(0, std::min<int64_t>(max_total, workflow_elapsed_ms_used));
    wf_elapsed_ms_remaining = std::max<int64_t>(0, max_total - used);
  }
  int64_t wf_total_tokens_remaining = 0;
  if (wf_limits.max_total_tokens > 0) {
    const int64_t max_total = wf_limits.max_total_tokens;
    const int64_t used = std::max<int64_t>(0, std::min<int64_t>(max_total, workflow_total_tokens_used));
    wf_total_tokens_remaining = std::max<int64_t>(0, max_total - used);
  }

  auto workflow_budget_exceeded_cancel = [&](const char* which) {
    const std::string msg = std::string("workflow budget exceeded: ") + (which ? which : "budget");
    AgentDb::WorkflowRow upd = wf_latest;
    upd.cancel_requested = true;
    upd.updated_unix_ms = now;
    upd.error = msg;
    (void)db_->upsert_workflow(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["reason"] = which ? which : "budget";
      d["max_tool_calls_total"] = (Json::Int64)wf_limits.max_tool_calls_total;
      d["tool_calls_used"] = (Json::Int64)workflow_tool_calls_used;
      d["tool_calls_remaining"] = (Json::Int64)wf_tool_calls_remaining;
      d["max_steps_total"] = (Json::Int64)wf_limits.max_steps_total;
      d["steps_used"] = (Json::Int64)workflow_steps_used;
      d["steps_remaining"] = (Json::Int64)wf_steps_remaining;
      d["max_elapsed_ms_total"] = (Json::Int64)wf_limits.max_elapsed_ms_total;
      d["elapsed_ms_used"] = (Json::Int64)workflow_elapsed_ms_used;
      d["elapsed_ms_remaining"] = (Json::Int64)wf_elapsed_ms_remaining;
      d["max_total_tokens"] = (Json::Int64)wf_limits.max_total_tokens;
      d["prompt_tokens_used"] = (Json::Int64)workflow_prompt_tokens_used;
      d["completion_tokens_used"] = (Json::Int64)workflow_completion_tokens_used;
      d["total_tokens_used"] = (Json::Int64)workflow_total_tokens_used;
      d["total_tokens_remaining"] = (Json::Int64)wf_total_tokens_remaining;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, task.task_id, "workflow_budget_exceeded", now, d);
    }

    // Bulk-cancel any still-queued tasks so the workflow can reach terminal state without scheduler thrash.
    // Important: do NOT clobber running tasks; only cancel if status is still queued.
    {
      std::vector<AgentDb::WorkflowTaskRow> tasks;
      std::string terr;
      if (db_ && db_->list_workflow_tasks(wf.workflow_id, &tasks, &terr)) {
        Json::Value jout(Json::objectValue);
        jout["ok"] = false;
        jout["cancelled"] = true;
        jout["assistant_text"] = "";
        jout["error"] = msg;
        jout["tool_calls_total"] = (Json::Int64)0;
        jout["steps_executed"] = (Json::Int64)0;
        jout["elapsed_ms"] = (Json::Int64)0;
        const std::string result_json = json_stringify_compact(jout);

        for (const auto& t : tasks) {
          if (t.task_id.empty()) continue;
          if (t.task_id == task.task_id) continue; // current claimed task handled below
          if (t.status != "queued") continue;
          std::string cerr;
          const bool cancelled = db_->cancel_workflow_task_if_queued(wf.workflow_id, t.task_id, now, msg, result_json, &cerr);
          if (!cancelled) continue;
          Json::Value d2(Json::objectValue);
          d2["workflow_id"] = wf.workflow_id;
          d2["task_id"] = t.task_id;
          d2["status"] = "cancelled";
          d2["attempt"] = t.attempt;
          d2["max_attempts"] = t.max_attempts;
          d2["reason"] = "budget_exceeded";
          d2["error"] = msg;
          d2["ts_unix_ms"] = (Json::Int64)now;
          insert_workflow_event_best_effort(db_, wf.workflow_id, t.task_id, "task_status", now, d2);
        }
      }
    }
    cancel_task_now(msg, "budget_exceeded");
  };

  // Build final run request body, applying template expansion.
  std::string request_body = task.request_json;
  Json::Value rr;
  std::string perr;
  Json::Value out;
  if (json_parse_any_value(task.request_json, &rr, &perr) && rr.isObject()) {
    std::vector<std::string> tmpl_errors;
    (void)workflow_expand_templates_for_task_request(&rr, assistant_by_task, result_json_by_task, &tmpl_errors);
    if (!tmpl_errors.empty()) {
      out = Json::Value(Json::objectValue);
      out["ok"] = false;
      out["assistant_text"] = "";
      out["error"] = "template expansion failed";
      Json::Value arr(Json::arrayValue);
      for (const auto& e : tmpl_errors) arr.append(e);
      out["template_errors"] = arr;
    }
    request_body = json_stringify_compact(rr);
  }

  const DaemonConfig cfg = cfg_snapshot_();
  const OpenAIClientConfig ocfg = ocfg_from_cfg_(cfg);
  std::string kind;
  if (rr.isObject()) kind = json_get_string(rr, "kind");
  if (!out.isNull()) {
    // Template expansion failed: do not execute provider calls. `out` is already populated as an error.
  } else if (kind == "avm_capsule") {
    const Json::Value cap = rr.isMember("capsule") && rr["capsule"].isObject() ? rr["capsule"] : Json::Value(Json::nullValue);
    Json::Value avm_out;
    std::string aerr;
    (void)avm_capsule_run_to_json(cfg, cap, &avm_out, &aerr);
    out = Json::Value(Json::objectValue);
    out["kind"] = "avm_capsule";
    out["avm"] = avm_out;
    const bool ok = avm_out.isObject() && avm_out.isMember("ok") && avm_out["ok"].isBool() && avm_out["ok"].asBool();
    out["ok"] = ok;
    if (!ok) {
      const std::string err =
        avm_out.isObject() && avm_out.isMember("error") && avm_out["error"].isString() ? avm_out["error"].asString()
        : (!aerr.empty() ? aerr : "avm capsule_run failed");
      out["error"] = err;
    }
    // For template expansion convenience, surface a bounded assistant_text (best-effort).
    if (avm_out.isObject() && avm_out.isMember("result_hash") && avm_out["result_hash"].isString()) {
      out["assistant_text"] = avm_out["result_hash"].asString();
    } else if (avm_out.isObject() && avm_out.isMember("run")) {
      out["assistant_text"] = json_stringify_compact_local(avm_out["run"]);
    } else {
      out["assistant_text"] = "";
    }
  } else if (kind == "aggregate") {
    const Json::Value agg = rr.isMember("aggregate") && rr["aggregate"].isObject() ? rr["aggregate"] : Json::Value(Json::nullValue);
    std::string aerr;
    out = workflow_aggregate_to_json(agg, result_json_by_task, &aerr);
    if (!aerr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = aerr;
  } else if (kind == "memory_put") {
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }
    out = workflow_memory_put_to_json(db_, cfg, sessions_root_dir_, wf, task, rr, &cancel_ctx, now);
  } else if (kind == "memory_consolidate") {
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }
    out = workflow_memory_consolidate_to_json(db_, cfg, wf, task, rr, &cancel_ctx, now);
  } else if (kind == "delay") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "delay";
    out["ok"] = false;

    int64_t delay_ms = 0;
    if (rr.isMember("delay_ms") && (rr["delay_ms"].isInt64() || rr["delay_ms"].isUInt64() || rr["delay_ms"].isInt())) {
      delay_ms = rr["delay_ms"].asInt64();
    } else if (rr.isMember("delay_ms")) {
      out["error"] = "delay_ms must be an integer";
    }

    if (!out.isMember("error")) {
      if (delay_ms < 0) {
        out["error"] = "delay_ms must be >= 0";
      } else {
        if (delay_ms > 600000) delay_ms = 600000;
        if (delay_ms > 0) {
          const int64_t until = unix_ms_now() + delay_ms;
          while (!stop_.load()) {
            if (workflow_run_should_cancel(&cancel_ctx)) {
              out["ok"] = false;
              out["cancelled"] = true;
              out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
              break;
            }
            const int64_t now2 = unix_ms_now();
            if (now2 >= until) break;
            const int64_t remaining = until - now2;
            const int64_t chunk = std::min<int64_t>(50, remaining);
            if (chunk > 0) std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
          }
        }

        if (!out.isMember("cancelled")) {
          out["ok"] = true;
          out["delay_ms"] = (Json::Int64)delay_ms;
        }

        if (out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool() && rr.isMember("result") && rr["result"].isObject()) {
          const auto& r = rr["result"];
          for (const auto& k : r.getMemberNames()) {
            if (k == "ok" || k == "kind" || k == "delay_ms") continue;
            out[k] = r[k];
          }
        } else if (rr.isMember("result") && !rr["result"].isNull()) {
          // Keep behavior strict: if provided, result must be an object.
          out["ok"] = false;
          out["error"] = "result must be an object";
        }

        if (out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool()) {
          if (!out.isMember("assistant_text") || !out["assistant_text"].isString()) {
            out["assistant_text"] = std::string("delay:") + std::to_string((long long)delay_ms);
          }
        }
      }
    }
  } else if (kind == "delegate") {
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "delegate";
    out["ok"] = false;

    const Json::Value del = rr.isMember("delegate") && rr["delegate"].isObject() ? rr["delegate"] : Json::Value(Json::nullValue);
    if (!del.isObject()) {
      out["error"] = "delegate missing delegate object";
    } else {
      const bool stop_on_ok =
        del.isMember("stop_on_ok") && del["stop_on_ok"].isBool() ? del["stop_on_ok"].asBool() : true;
      const Json::Value attempt_caps =
        del.isMember("attempt_caps") && del["attempt_caps"].isObject() ? del["attempt_caps"] : Json::Value(Json::nullValue);

      const Json::Value attempts = del.isMember("attempts") && del["attempts"].isArray() ? del["attempts"] : Json::Value(Json::nullValue);
      if (!attempts.isArray() || attempts.empty()) {
        out["error"] = "delegate.attempts must be a non-empty array";
      } else {
        auto clamp_text = [](const std::string& s, size_t max_chars) -> std::string {
          if (s.size() <= max_chars) return s;
          return s.substr(0, max_chars);
        };

        Json::Value del_out(Json::objectValue);
        del_out["stop_on_ok"] = stop_on_ok;
        Json::Value arr(Json::arrayValue);

        bool any_ok = false;
        std::string chosen_id;
        std::string chosen_text;
        std::string last_err;

	        int64_t attempts_tool_calls_total = 0;
	        int64_t attempts_steps_executed = 0;
	        int64_t attempts_elapsed_ms = 0;
	        int64_t attempts_prompt_tokens = 0;
	        int64_t attempts_completion_tokens = 0;
	        int64_t attempts_total_tokens = 0;
	        int64_t remaining_local = wf_tool_calls_remaining;
	        int64_t remaining_steps_local = wf_steps_remaining;
	        int64_t remaining_elapsed_ms_local = wf_elapsed_ms_remaining;
	        int64_t remaining_tokens_local = wf_total_tokens_remaining;

	        for (Json::ArrayIndex i = 0; i < attempts.size(); i++) {
	          if (workflow_run_should_cancel(&cancel_ctx)) {
	            out["cancelled"] = true;
	            out["ok"] = false;
	            out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
	            break;
	          }

	          if (wf_limits.max_tool_calls_total > 0 && remaining_local <= 0) {
	            out["ok"] = false;
	            out["assistant_text"] = "";
	            out["error"] = "workflow budget exceeded: max_tool_calls_total";
	            break;
	          }
	          if (wf_limits.max_steps_total > 0 && remaining_steps_local <= 0) {
	            out["ok"] = false;
	            out["assistant_text"] = "";
	            out["error"] = "workflow budget exceeded: max_steps_total";
	            break;
	          }
	          if (wf_limits.max_elapsed_ms_total > 0 && remaining_elapsed_ms_local <= 0) {
	            out["ok"] = false;
	            out["assistant_text"] = "";
	            out["error"] = "workflow budget exceeded: max_elapsed_ms_total";
	            break;
	          }
	          if (wf_limits.max_total_tokens > 0 && remaining_tokens_local <= 0) {
	            out["ok"] = false;
	            out["assistant_text"] = "";
	            out["error"] = "workflow budget exceeded: max_total_tokens";
	            break;
	          }

	          const Json::Value a = attempts[i];
	          if (!a.isObject()) continue;

          const std::string aid = a.isMember("id") && a["id"].isString() ? a["id"].asString() : ("att_" + std::to_string((int)i));
          const Json::Value areq = a.isMember("request") && a["request"].isObject() ? a["request"] : Json::Value(Json::nullValue);
          if (!areq.isObject()) {
            Json::Value row(Json::objectValue);
            row["id"] = aid;
            row["ok"] = false;
            row["run_ok"] = false;
            row["expect_ok"] = true;
            row["error"] = "delegate attempt missing request object";
            arr.append(row);
            last_err = "delegate attempt missing request object";
            continue;
          }

	          Json::Value areq2 = areq;

	          // attempt_caps: hard maximums on per-attempt run knobs.
	          // Semantics: if cap <= 0, ignore. Else: missing => set to cap; present => min(present, cap).
	          if (attempt_caps.isObject()) {
	            auto clamp_key = [&](const char* k) {
	              if (!attempt_caps.isMember(k)) return;
	              const Json::Value& capv = attempt_caps[k];
	              if (!(capv.isInt64() || capv.isUInt64() || capv.isInt() || capv.isUInt())) return;
	              const int64_t cap = std::max<int64_t>(0, capv.asInt64());
	              if (cap <= 0) return;
	              if (areq2.isMember(k) && (areq2[k].isInt64() || areq2[k].isUInt64() || areq2[k].isInt() || areq2[k].isUInt())) {
	                const int64_t cur = std::max<int64_t>(0, areq2[k].asInt64());
	                areq2[k] = (Json::Int64)std::min<int64_t>(cur, cap);
	              } else if (!areq2.isMember(k)) {
	                areq2[k] = (Json::Int64)cap;
	              }
	            };
	            clamp_key("timeout_ms");
	            clamp_key("max_steps");
	            clamp_key("max_tool_calls_total");
	            clamp_key("max_tool_calls_per_tool");
	          }

	          if (wf_limits.max_tool_calls_total > 0) {
	            // Clamp per-attempt tool-call budget to remaining workflow budget.
	            int64_t req_limit = 0;
	            if (areq2.isMember("max_tool_calls_total") && (areq2["max_tool_calls_total"].isInt64() || areq2["max_tool_calls_total"].isUInt64() || areq2["max_tool_calls_total"].isInt() || areq2["max_tool_calls_total"].isUInt())) {
	              req_limit = std::max<int64_t>(0, areq2["max_tool_calls_total"].asInt64());
	            }
	            const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, remaining_local) : remaining_local;
	            areq2["max_tool_calls_total"] = (Json::Int64)std::max<int64_t>(0, eff);
	          }
	          if (wf_limits.max_steps_total > 0) {
	            int64_t req_limit = 0;
	            if (areq2.isMember("max_steps") && (areq2["max_steps"].isInt64() || areq2["max_steps"].isUInt64() || areq2["max_steps"].isInt() || areq2["max_steps"].isUInt())) {
	              req_limit = std::max<int64_t>(0, areq2["max_steps"].asInt64());
	            }
	            const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, remaining_steps_local) : remaining_steps_local;
	            areq2["max_steps"] = (Json::Int64)std::max<int64_t>(0, eff);
	          }
	          if (wf_limits.max_elapsed_ms_total > 0) {
	            int64_t req_limit = 0;
	            if (areq2.isMember("timeout_ms") && (areq2["timeout_ms"].isInt64() || areq2["timeout_ms"].isUInt64() || areq2["timeout_ms"].isInt() || areq2["timeout_ms"].isUInt())) {
	              req_limit = std::max<int64_t>(0, areq2["timeout_ms"].asInt64());
	            }
	            const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, remaining_elapsed_ms_local) : remaining_elapsed_ms_local;
	            areq2["timeout_ms"] = (Json::Int64)std::max<int64_t>(0, eff);
	          }
	          const std::string attempt_body = json_stringify_compact(areq2);
	          Json::Value r = run_request_to_json_internal_cancellable(
	            cfg,
	            ocfg,
	            db_,
            tool_ext_or_null_,
            sessions_root_dir_,
            attempt_body,
            nullptr,
            workflow_run_should_cancel,
            &cancel_ctx
          );

          std::string expect_err2;
          bool expect_ok2 = true;
          if (a.isMember("expect") && a["expect"].isObject()) {
            const std::string expect_json2 = json_stringify_compact(a["expect"]);
            expect_ok2 = apply_expectations(expect_json2, r, &expect_err2);
          }

	          const bool run_ok2 = r.isObject() && r.isMember("ok") && r["ok"].isBool() && r["ok"].asBool();
	          const bool ok2 = run_ok2 && expect_ok2;
	          const std::string atext = clamp_text(json_get_string(r, "assistant_text"), 8192);

	          int64_t tool_calls_this_attempt = 0;
	          if (r.isObject() && r.isMember("tool_calls_total") && (r["tool_calls_total"].isInt64() || r["tool_calls_total"].isUInt64() || r["tool_calls_total"].isInt() || r["tool_calls_total"].isUInt())) {
	            tool_calls_this_attempt = std::max<int64_t>(0, r["tool_calls_total"].asInt64());
	          }
	          int64_t steps_this_attempt = 0;
	          if (r.isObject() && r.isMember("steps_executed") && (r["steps_executed"].isInt64() || r["steps_executed"].isUInt64() || r["steps_executed"].isInt() || r["steps_executed"].isUInt())) {
	            steps_this_attempt = std::max<int64_t>(0, r["steps_executed"].asInt64());
	          }
	          int64_t elapsed_ms_this_attempt = 0;
	          if (r.isObject() && r.isMember("elapsed_ms") && (r["elapsed_ms"].isInt64() || r["elapsed_ms"].isUInt64() || r["elapsed_ms"].isInt() || r["elapsed_ms"].isUInt())) {
	            elapsed_ms_this_attempt = std::max<int64_t>(0, r["elapsed_ms"].asInt64());
	          }
	          int64_t prompt_tokens_this_attempt = 0;
	          if (r.isObject() && r.isMember("prompt_tokens") && (r["prompt_tokens"].isInt64() || r["prompt_tokens"].isUInt64() || r["prompt_tokens"].isInt() || r["prompt_tokens"].isUInt())) {
	            prompt_tokens_this_attempt = std::max<int64_t>(0, r["prompt_tokens"].asInt64());
	          }
	          int64_t completion_tokens_this_attempt = 0;
	          if (r.isObject() && r.isMember("completion_tokens") && (r["completion_tokens"].isInt64() || r["completion_tokens"].isUInt64() || r["completion_tokens"].isInt() || r["completion_tokens"].isUInt())) {
	            completion_tokens_this_attempt = std::max<int64_t>(0, r["completion_tokens"].asInt64());
	          }
	          int64_t total_tokens_this_attempt = 0;
	          if (r.isObject() && r.isMember("total_tokens") && (r["total_tokens"].isInt64() || r["total_tokens"].isUInt64() || r["total_tokens"].isInt() || r["total_tokens"].isUInt())) {
	            total_tokens_this_attempt = std::max<int64_t>(0, r["total_tokens"].asInt64());
	          } else if (prompt_tokens_this_attempt > 0 || completion_tokens_this_attempt > 0) {
	            total_tokens_this_attempt = prompt_tokens_this_attempt + completion_tokens_this_attempt;
	          }
	          attempts_tool_calls_total = saturating_add_i64(attempts_tool_calls_total, tool_calls_this_attempt);
	          attempts_steps_executed = saturating_add_i64(attempts_steps_executed, steps_this_attempt);
	          attempts_elapsed_ms = saturating_add_i64(attempts_elapsed_ms, elapsed_ms_this_attempt);
	          attempts_prompt_tokens = saturating_add_i64(attempts_prompt_tokens, prompt_tokens_this_attempt);
	          attempts_completion_tokens = saturating_add_i64(attempts_completion_tokens, completion_tokens_this_attempt);
	          attempts_total_tokens = saturating_add_i64(attempts_total_tokens, total_tokens_this_attempt);
	          if (wf_limits.max_tool_calls_total > 0) {
	            remaining_local = std::max<int64_t>(0, remaining_local - tool_calls_this_attempt);
	          }
	          if (wf_limits.max_steps_total > 0) {
	            remaining_steps_local = std::max<int64_t>(0, remaining_steps_local - steps_this_attempt);
	          }
	          if (wf_limits.max_elapsed_ms_total > 0) {
	            remaining_elapsed_ms_local = std::max<int64_t>(0, remaining_elapsed_ms_local - elapsed_ms_this_attempt);
	          }
	          if (wf_limits.max_total_tokens > 0) {
	            remaining_tokens_local = std::max<int64_t>(0, remaining_tokens_local - total_tokens_this_attempt);
	          }

	          Json::Value row(Json::objectValue);
	          row["id"] = aid;
	          row["ok"] = ok2;
	          row["run_ok"] = run_ok2;
	          row["expect_ok"] = expect_ok2;

	          auto copy_eff_i64 = [&](const char* k) {
	            if (!r.isObject()) return;
	            if (r.isMember(k) && (r[k].isInt64() || r[k].isUInt64() || r[k].isInt() || r[k].isUInt())) {
	              row[k] = (Json::Int64)std::max<int64_t>(0, r[k].asInt64());
	            }
	          };
	          copy_eff_i64("effective_timeout_ms");
	          copy_eff_i64("effective_max_steps");
	          copy_eff_i64("effective_max_tool_calls_total");
	          copy_eff_i64("effective_max_tool_calls_per_tool");

	          row["tool_calls_total"] = (Json::Int64)tool_calls_this_attempt;
	          row["steps_executed"] = (Json::Int64)steps_this_attempt;
	          row["elapsed_ms"] = (Json::Int64)elapsed_ms_this_attempt;
	          row["prompt_tokens"] = (Json::Int64)prompt_tokens_this_attempt;
	          row["completion_tokens"] = (Json::Int64)completion_tokens_this_attempt;
	          row["total_tokens"] = (Json::Int64)total_tokens_this_attempt;
	          if (!atext.empty()) row["assistant_text"] = atext;
	          const std::string err = json_get_string(r, "error");
	          if (!err.empty()) row["error"] = err;
	          if (!expect_ok2) row["expect_error"] = expect_err2;
          if (r.isObject() && r.isMember("http_status") && (r["http_status"].isInt() || r["http_status"].isInt64())) {
            row["http_status"] = r["http_status"];
          }
          arr.append(row);

          if (!err.empty()) last_err = err;
          if (!expect_ok2 && last_err.empty()) last_err = expect_err2;

          if (ok2 && !any_ok) {
            any_ok = true;
            chosen_id = aid;
            chosen_text = atext;
          }

          if (ok2 && stop_on_ok) break;
        }

	        del_out["attempts"] = arr;
	        del_out["attempts_total"] = (Json::Int64)attempts.size();
	        del_out["attempts_run"] = (Json::Int64)arr.size();
	        if (!chosen_id.empty()) del_out["chosen_id"] = chosen_id;
	        out["delegate"] = del_out;
	        out["tool_calls_total"] = (Json::Int64)attempts_tool_calls_total;
	        out["steps_executed"] = (Json::Int64)attempts_steps_executed;
	        out["elapsed_ms"] = (Json::Int64)attempts_elapsed_ms;
	        out["prompt_tokens"] = (Json::Int64)attempts_prompt_tokens;
	        out["completion_tokens"] = (Json::Int64)attempts_completion_tokens;
	        out["total_tokens"] = (Json::Int64)attempts_total_tokens;

	        if (out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool()) {
	          // already populated
	        } else if (any_ok) {
	          out["ok"] = true;
          out["assistant_text"] = chosen_text;
        } else {
          out["ok"] = false;
          out["assistant_text"] = "";
          out["error"] = last_err.empty() ? "delegate attempts all failed" : last_err;
        }
      }
    }
  } else if (kind == "memory_search") {
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_search";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else if (lower_copy(trim_copy(cfg.tools)) != "host") {
      out["error"] = "memory_search requires --tools host";
    } else {
      // Note: memory_search is read-only; allow host_policy=readonly or full.
      const Json::Value ms =
        rr.isMember("memory_search") && rr["memory_search"].isObject() ? rr["memory_search"] : Json::Value(Json::nullValue);
      if (!ms.isObject()) {
        out["error"] = "memory_search missing memory_search object";
      } else {
        HostToolsetConfig hcfg;
        hcfg.root_dir = cfg.state_dir;
        hcfg.policy = cfg.host_policy;
        hcfg.enable_process_exec = false;
        hcfg.allow_symlinks = true;
        hcfg.sessions_root_dir = !cfg.sessions_root_dir.empty() ? cfg.sessions_root_dir : sessions_root_dir_;
        hcfg.session_id = wf.session_id;

        agent_tool_registry_t* reg = nullptr;
        agent_tool_executor_t exec{};
        const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
        if (st != AGENT_OK || !reg || !exec.execute) {
          if (reg) agent_tool_registry_destroy(reg);
          toolset_host_destroy(&exec);
          out["error"] = "failed to create host toolset";
        } else {
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          const std::string req = Json::writeString(wb, ms);

          agent_string_t out_s{};
          // Budget charging: deterministic host-tool invocation. Count it as one tool call / step.
          const agent_status_t est = exec.execute(exec.ctx, "memory_search", req.c_str(), &out_s);
          out["tool_calls_total"] = (Json::Int64)1;
          out["steps_executed"] = (Json::Int64)1;
          const std::string resp_s = (out_s.data && out_s.len) ? std::string(out_s.data, out_s.len) : std::string();
          agent_string_free(&out_s);

          agent_tool_registry_destroy(reg);
          toolset_host_destroy(&exec);

          if (est != AGENT_OK) {
            out["error"] = "memory_search failed";
          } else {
            Json::Value resp(Json::objectValue);
            std::string rerr;
            if (!json_parse_any_value(resp_s, &resp, &rerr) || !resp.isObject()) {
              out["error"] = "failed to parse memory_search response";
              out["parse_error"] = rerr;
            } else {
              out["memory_search_response"] = resp;
              const bool ok = resp.isMember("ok") && resp["ok"].isBool() && resp["ok"].asBool();
              out["ok"] = ok;
              if (!ok) {
                const std::string err =
                  resp.isMember("error") && resp["error"].isString() ? resp["error"].asString() : "memory_search failed";
                out["error"] = err;
                out["assistant_text"] = "";
              } else {
                if (resp.isMember("data") && resp["data"].isObject() && resp["data"].isMember("output") && resp["data"]["output"].isString()) {
                  out["assistant_text"] = resp["data"]["output"].asString();
                } else {
                  out["assistant_text"] = "memory_search: ok";
                }
              }
            }
          }
        }
      }
    }
  } else if (kind == "memory_structured_query") {
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_structured_query";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else if (lower_copy(trim_copy(cfg.tools)) != "host") {
      out["error"] = "memory_structured_query requires --tools host";
    } else {
      // Note: memory_structured_query is read-only; allow host_policy=readonly or full.
      const Json::Value msq =
        rr.isMember("memory_structured_query") && rr["memory_structured_query"].isObject() ? rr["memory_structured_query"] : Json::Value(Json::nullValue);
      if (!msq.isObject()) {
        out["error"] = "memory_structured_query missing memory_structured_query object";
      } else {
        HostToolsetConfig hcfg;
        hcfg.root_dir = cfg.state_dir;
        hcfg.policy = cfg.host_policy;
        hcfg.enable_process_exec = false;
        hcfg.allow_symlinks = true;
        hcfg.sessions_root_dir = !cfg.sessions_root_dir.empty() ? cfg.sessions_root_dir : sessions_root_dir_;
        hcfg.session_id = wf.session_id;

        agent_tool_registry_t* reg = nullptr;
        agent_tool_executor_t exec{};
        const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
        if (st != AGENT_OK || !reg || !exec.execute) {
          if (reg) agent_tool_registry_destroy(reg);
          toolset_host_destroy(&exec);
          out["error"] = "failed to create host toolset";
        } else {
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          const std::string req = Json::writeString(wb, msq);

          agent_string_t out_s{};
          // Budget charging: deterministic host-tool invocation. Count it as one tool call / step.
          const agent_status_t est = exec.execute(exec.ctx, "memory_structured_query", req.c_str(), &out_s);
          out["tool_calls_total"] = (Json::Int64)1;
          out["steps_executed"] = (Json::Int64)1;
          const std::string resp_s = (out_s.data && out_s.len) ? std::string(out_s.data, out_s.len) : std::string();
          agent_string_free(&out_s);

          agent_tool_registry_destroy(reg);
          toolset_host_destroy(&exec);

          if (est != AGENT_OK) {
            out["error"] = "memory_structured_query failed";
          } else {
            Json::Value resp(Json::objectValue);
            std::string rerr;
            if (!json_parse_any_value(resp_s, &resp, &rerr) || !resp.isObject()) {
              out["error"] = "failed to parse memory_structured_query response";
              out["parse_error"] = rerr;
            } else {
              out["memory_structured_query_response"] = resp;
              const bool ok = resp.isMember("ok") && resp["ok"].isBool() && resp["ok"].asBool();
              out["ok"] = ok;
              if (!ok) {
                const std::string err =
                  resp.isMember("error") && resp["error"].isString() ? resp["error"].asString() : "memory_structured_query failed";
                out["error"] = err;
                out["assistant_text"] = "";
              } else {
                if (resp.isMember("data") && resp["data"].isObject() && resp["data"].isMember("output") && resp["data"]["output"].isString()) {
                  out["assistant_text"] = resp["data"]["output"].asString();
                } else {
                  out["assistant_text"] = "memory_structured_query: ok";
                }
              }
            }
          }
        }
      }
    }
  } else if (kind == "memory_correlate") {
    // Deterministic correlation query over structured memory checkpoints (no LLM required).
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_correlate";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else {
      const Json::Value mc =
        rr.isMember("memory_correlate") && rr["memory_correlate"].isObject() ? rr["memory_correlate"] : Json::Value(Json::nullValue);
      if (!mc.isObject()) {
        out["error"] = "memory_correlate missing memory_correlate object";
      } else {
        std::string merr;
        out = workflow_memory_correlate_to_json(cfg.state_dir, wf.trace_id, mc, &merr);
        out["kind"] = "memory_correlate";
        if (!merr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = merr;
      }
    }
  } else if (kind == "memory_query") {
    // Deterministic structured memory query over checkpoints (no LLM required).
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_query";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else {
      const Json::Value mq =
        rr.isMember("memory_query") && rr["memory_query"].isObject() ? rr["memory_query"] : Json::Value(Json::nullValue);
      if (!mq.isObject()) {
        out["error"] = "memory_query missing memory_query object";
      } else {
        std::string merr;
        out = workflow_memory_query_to_json(cfg.state_dir, mq, &merr);
        out["kind"] = "memory_query";
        if (!merr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = merr;
      }
    }
  } else if (kind == "http_json") {
    // Deterministic outbound HTTP JSON call (broker/agent interop; gated by daemon config).
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "http_json";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else {
      const Json::Value hj =
        rr.isMember("http_json") && rr["http_json"].isObject() ? rr["http_json"] : Json::Value(Json::nullValue);
      if (!hj.isObject()) {
        out["error"] = "http_json missing http_json object";
      } else {
        std::string herr;
        out = workflow_http_json_to_json(cfg, hj, &herr);
        out["kind"] = "http_json";
        // Budget charging: deterministic external call. Count as one tool call / step per attempt.
        if (!out.isMember("tool_calls_total")) out["tool_calls_total"] = (Json::Int64)1;
        if (!out.isMember("steps_executed")) out["steps_executed"] = (Json::Int64)1;
        if (!herr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = herr;
      }
    }
  } else if (kind == "agentd_call") {
    // Deterministic agent-to-agent collaboration (submit+poll remote agentd workflow; gated by daemon config).
    if (wf_limits.max_tool_calls_total > 0 && wf_tool_calls_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_tool_calls_total");
      return;
    }
    if (wf_limits.max_steps_total > 0 && wf_steps_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_steps_total");
      return;
    }
    if (wf_limits.max_elapsed_ms_total > 0 && wf_elapsed_ms_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_elapsed_ms_total");
      return;
    }
    if (wf_limits.max_total_tokens > 0 && wf_total_tokens_remaining <= 0) {
      workflow_budget_exceeded_cancel("max_total_tokens");
      return;
    }

    out = Json::Value(Json::objectValue);
    out["kind"] = "agentd_call";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else {
      const Json::Value ac =
        rr.isMember("agentd_call") && rr["agentd_call"].isObject() ? rr["agentd_call"] : Json::Value(Json::nullValue);
      if (!ac.isObject()) {
        out["error"] = "agentd_call missing agentd_call object";
      } else {
        const std::string ttrace =
          rr.isMember("trace_id") && rr["trace_id"].isString() ? trim_copy(rr["trace_id"].asString()) : "";
        std::string aerr;
        out = workflow_agentd_call_to_json(cfg, ac, ttrace, task.result_json, &aerr);
        out["kind"] = "agentd_call";
        // Budget charging: deterministic external calls. Count as one tool call / step per attempt.
        if (!out.isMember("tool_calls_total")) out["tool_calls_total"] = (Json::Int64)1;
        if (!out.isMember("steps_executed")) out["steps_executed"] = (Json::Int64)1;
        if (!aerr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = aerr;
      }
    }
  } else if (kind == "edge_invoke") {
    Json::Value e = rr.isMember("edge") && rr["edge"].isObject() ? rr["edge"] : Json::Value(Json::nullValue);
    out = Json::Value(Json::objectValue);
    out["kind"] = "edge_invoke";
    out["ok"] = false;

    // Best-effort: reuse chosen node_id from a previous attempt to avoid cross-node duplication.
    if ((!e.isObject() || !e.isMember("node_id") || !e["node_id"].isString() || e["node_id"].asString().empty()) && !task.result_json.empty()) {
      Json::Value prev;
      std::string perr2;
      if (json_parse_any_value(task.result_json, &prev, &perr2) && prev.isObject()) {
        const std::string prev_node =
          prev.isMember("edge") && prev["edge"].isObject() && prev["edge"].isMember("node_id") && prev["edge"]["node_id"].isString()
          ? prev["edge"]["node_id"].asString()
          : "";
        if (!prev_node.empty()) {
          if (!e.isObject()) e = Json::Value(Json::objectValue);
          e["node_id"] = prev_node;
        }
      }
    }

    if (!db_ || !db_->is_open()) {
      out["error"] = "db not available";
      out["retryable"] = true;
      out["retry_in_ms"] = 250;
    } else if (!e.isObject()) {
      out["error"] = "edge config must be an object";
    } else {
      std::string node_id = e.isMember("node_id") && e["node_id"].isString() ? e["node_id"].asString() : "";

      if (node_id.empty() && e.isMember("match_any") && e["match_any"].isObject()) {
        const auto& m = e["match_any"];
        auto read_arr = [&](const char* k, std::vector<std::string>* outv) {
          if (!outv) return;
          outv->clear();
          if (!m.isMember(k) || !m[k].isArray()) return;
          for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
            if (!m[k][i].isString()) continue;
            const std::string s = m[k][i].asString();
            if (!s.empty()) outv->push_back(s);
          }
        };
        std::vector<std::string> requires_tools;
        std::vector<std::string> tags_all;
        std::vector<std::string> tags_any;
        std::vector<std::string> tags_none;
        std::unordered_set<std::string> exclude_node_ids;
        read_arr("requires_tools", &requires_tools);
        read_arr("tags_all", &tags_all);
        read_arr("tags_any", &tags_any);
        read_arr("tags_none", &tags_none);
        if (m.isMember("exclude_node_ids") && m["exclude_node_ids"].isArray()) {
          for (Json::ArrayIndex i = 0; i < m["exclude_node_ids"].size(); i++) {
            if (!m["exclude_node_ids"][i].isString()) continue;
            const std::string s = trim_copy(m["exclude_node_ids"][i].asString());
            if (!s.empty()) exclude_node_ids.insert(s);
          }
        }
        const std::unordered_set<std::string>* ex = exclude_node_ids.empty() ? nullptr : &exclude_node_ids;
        (void)edge_select_node_match_any(db_, requires_tools, tags_all, tags_any, tags_none, ex, &node_id);
        if (!node_id.empty()) e["node_id"] = node_id;
      }

      const std::string mode = e.isMember("mode") && e["mode"].isString() ? e["mode"].asString() : "invoke";
      const std::string tool_name = e.isMember("tool") && e["tool"].isString() ? e["tool"].asString() : "";
      const Json::Value args = e.isMember("args") && e["args"].isObject() ? e["args"] : Json::Value(Json::nullValue);

      int64_t deadline_utc_ms = 0;
      if (e.isMember("deadline_utc_ms") && (e["deadline_utc_ms"].isInt64() || e["deadline_utc_ms"].isUInt64())) {
        deadline_utc_ms = e["deadline_utc_ms"].isInt64() ? e["deadline_utc_ms"].asInt64() : (int64_t)e["deadline_utc_ms"].asUInt64();
      } else if (e.isMember("timeout_ms") && (e["timeout_ms"].isInt64() || e["timeout_ms"].isUInt64() || e["timeout_ms"].isInt())) {
        const int64_t tmo = e["timeout_ms"].asInt64();
        const int64_t tmo2 = std::max<int64_t>(100, std::min<int64_t>(600000, tmo));
        deadline_utc_ms = edge_unix_ms_now() + tmo2;
      } else {
        deadline_utc_ms = edge_unix_ms_now() + 5000;
      }

      std::unordered_set<std::string> allow_hazards;
      if (e.isMember("allow_hazards") && e["allow_hazards"].isArray()) {
        for (Json::ArrayIndex i = 0; i < e["allow_hazards"].size(); i++) {
          if (e["allow_hazards"][i].isString()) allow_hazards.insert(e["allow_hazards"][i].asString());
        }
      }
      const bool allow_high_side_effect =
        e.isMember("allow_high_side_effect") && e["allow_high_side_effect"].isBool() ? e["allow_high_side_effect"].asBool() : false;

      std::string idempotency_key = e.isMember("idempotency_key") && e["idempotency_key"].isString() ? e["idempotency_key"].asString() : "";
      if (idempotency_key.empty()) idempotency_key = wf.workflow_id + ":" + task.task_id;

      if (node_id.empty()) {
        out["error"] = "missing node_id (or match_any did not select)";
      } else {
        Json::Value payload(Json::objectValue);
        if (mode == "invoke") {
          if (tool_name.empty() || !args.isObject()) {
            out["error"] = "missing tool/args";
            out["edge"] = e;
            // fall through to persist out
            payload = Json::Value(Json::nullValue);
          } else {
            payload["tool"] = tool_name;
            payload["args"] = args;
          }
        } else if (mode == "agent") {
          if (e.isMember("payload") && e["payload"].isObject()) {
            payload = e["payload"];
          } else if (e.isMember("prompt") && e["prompt"].isString() && !e["prompt"].asString().empty()) {
            payload["prompt"] = e["prompt"].asString();
          } else {
            out["error"] = "missing edge.payload (object) or edge.prompt (string) for mode=agent";
            out["edge"] = e;
            payload = Json::Value(Json::nullValue);
          }
        } else {
          out["error"] = "unsupported edge mode (expected invoke|agent)";
          out["edge"] = e;
          payload = Json::Value(Json::nullValue);
        }

        if (!payload.isObject()) {
          // Invalid payload/mode; no enqueue.
        } else {
          // Enqueue TASK_ASSIGN (idempotent via UNIQUE(node_id,idempotency_key) and PK(task_id,step_id)).

          int64_t outbox_id = 0;
          bool deduped = false;
          std::string derr;
          int http = 500;
          Json::Value trace(Json::nullValue);
          if (!wf.trace_id.empty()) {
            trace = Json::Value(Json::objectValue);
            trace["trace_id"] = wf.trace_id;
          }
          const bool enq_ok = edge_enqueue_task_assign(
            db_,
            node_id,
            wf.workflow_id,
            task.task_id,
            idempotency_key,
            mode,
            deadline_utc_ms,
            task.attempt,
            payload,
            trace,
            allow_hazards,
            allow_high_side_effect,
            /*enforce_safety=*/true,
            /*enforce_rate_limit=*/true,
            &outbox_id,
            &deduped,
            &derr,
            &http);

          Json::Value edge(Json::objectValue);
          edge["node_id"] = node_id;
          edge["task_id"] = wf.workflow_id;
          edge["step_id"] = task.task_id;
          edge["idempotency_key"] = idempotency_key;
          edge["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
          edge["outbox_id"] = (Json::Int64)outbox_id;
          edge["deduped"] = deduped;
          edge["mode"] = mode;
          out["edge"] = edge;

          if (!enq_ok) {
            out["error"] = derr.empty() ? "failed to enqueue edge task" : derr;
              } else {
                AgentDb::EdgeTaskRow tr;
                std::string terr;
                if (!db_->get_edge_task(wf.workflow_id, task.task_id, &tr, &terr)) {
                  out["error"] = "edge task not found after enqueue";
                  out["retryable"] = true;
                  out["retry_in_ms"] = 100;
                } else {
                  out["edge_state"] = tr.state;
                  if (!tr.result_sha256.empty()) out["edge_result_sha256"] = tr.result_sha256;
                  if (!tr.result_json.empty()) {
                    Json::Value v;
                    std::string perr3;
                    if (json_parse_any(tr.result_json, &v, &perr3)) out["edge_result"] = v;
                  }
                  if (!tr.attest_json.empty()) {
                    Json::Value v;
                    std::string perr3;
                    if (json_parse_any(tr.attest_json, &v, &perr3) && v.isObject()) out["edge_attest"] = v;
                  }
                  if (!tr.error.empty()) out["edge_error"] = tr.error;

                  if (tr.state == "SUCCEEDED") {
                    out["ok"] = true;
                    if (out.isMember("edge_result")) out["assistant_text"] = json_stringify_compact_local(out["edge_result"]);
                else out["assistant_text"] = "edge:SUCCEEDED";
              } else if (tr.state == "FAILED") {
                out["ok"] = false;
                out["error"] = tr.error.empty() ? "edge task failed" : tr.error;
              } else {
                out["ok"] = false;
                out["error"] = "edge task pending";
                out["retryable"] = true;
                out["retry_in_ms"] = 100;
                out["assistant_text"] = "edge:" + tr.state;
              }
            }
          }
        }
      }
    }
  } else if (kind == "edge_wait_sensor") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "edge_wait_sensor";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (!db_ || !db_->is_open()) {
      out["error"] = "db not available";
      out["retryable"] = true;
      out["retry_in_ms"] = 250;
    } else if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else {
      const Json::Value spec =
        rr.isMember("edge_wait_sensor") && rr["edge_wait_sensor"].isObject() ? rr["edge_wait_sensor"] : Json::Value(Json::nullValue);
      if (!spec.isObject()) {
        out["error"] = "edge_wait_sensor missing edge_wait_sensor object";
      } else {
        const std::string event_type =
          spec.isMember("event_type") && spec["event_type"].isString() ? trim_copy(spec["event_type"].asString()) : "";
        const std::string node_id =
          spec.isMember("node_id") && spec["node_id"].isString() ? trim_copy(spec["node_id"].asString()) : "";

        double min_conf = 0.0;
        if (spec.isMember("min_confidence") && (spec["min_confidence"].isDouble() || spec["min_confidence"].isInt())) {
          min_conf = spec["min_confidence"].asDouble();
        }
        if (min_conf < 0.0) min_conf = 0.0;
        if (min_conf > 1.0) min_conf = 1.0;

        int64_t since_utc_ms = 0;
        if (spec.isMember("since_utc_ms") && (spec["since_utc_ms"].isInt64() || spec["since_utc_ms"].isUInt64() || spec["since_utc_ms"].isInt())) {
          since_utc_ms = spec["since_utc_ms"].asInt64();
        } else {
          // Default since time: workflow creation, so old sensor events don't accidentally satisfy a new workflow.
          since_utc_ms = wf.created_unix_ms;
        }
        if (since_utc_ms < 0) since_utc_ms = 0;

        int poll_ms = 250;
        if (spec.isMember("poll_ms") && (spec["poll_ms"].isInt() || spec["poll_ms"].isUInt())) {
          poll_ms = spec["poll_ms"].asInt();
        }
        poll_ms = std::max(10, std::min(60000, poll_ms));

        if (event_type.empty()) {
          out["error"] = "edge_wait_sensor.event_type is required";
        } else {
          AgentDb::EdgeSensorEventRow ev;
          bool found = false;
          std::string err;
          if (!db_->find_edge_sensor_event_latest(event_type, node_id, since_utc_ms, min_conf, &ev, &found, &err)) {
            out["error"] = err.empty() ? "failed to query edge sensor events" : err;
            out["retryable"] = true;
            out["retry_in_ms"] = poll_ms;
          } else if (!found || ev.id <= 0) {
            out["error"] = "sensor event pending";
            out["retryable"] = true;
            out["retry_in_ms"] = poll_ms;
            out["assistant_text"] = "edge_wait_sensor:pending";
          } else {
            out["ok"] = true;
            out["assistant_text"] = event_type;
            Json::Value o(Json::objectValue);
            o["id"] = (Json::Int64)ev.id;
            o["node_id"] = ev.node_id;
            o["event_type"] = ev.event_type;
            o["ts_utc_ms"] = (Json::Int64)ev.ts_utc_ms;
            o["confidence"] = ev.confidence;
            if (!ev.data_json.empty()) {
              Json::Value d;
              std::string perr2;
              if (json_parse_any(ev.data_json, &d, &perr2) && d.isObject()) o["data"] = d;
            }
            out["edge_sensor_event"] = o;
          }
        }
      }
    }
  } else {
    if (wf_limits.max_tool_calls_total > 0) {
      if (wf_tool_calls_remaining <= 0) {
        workflow_budget_exceeded_cancel("max_tool_calls_total");
        return;
      }
      if (rr.isObject()) {
        int64_t req_limit = 0;
        if (rr.isMember("max_tool_calls_total") && (rr["max_tool_calls_total"].isInt64() || rr["max_tool_calls_total"].isUInt64() || rr["max_tool_calls_total"].isInt() || rr["max_tool_calls_total"].isUInt())) {
          req_limit = std::max<int64_t>(0, rr["max_tool_calls_total"].asInt64());
        }
        const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, wf_tool_calls_remaining) : wf_tool_calls_remaining;
        rr["max_tool_calls_total"] = (Json::Int64)std::max<int64_t>(0, eff);
        request_body = json_stringify_compact(rr);
      }
    }
    if (wf_limits.max_steps_total > 0) {
      if (wf_steps_remaining <= 0) {
        workflow_budget_exceeded_cancel("max_steps_total");
        return;
      }
      if (rr.isObject()) {
        int64_t req_limit = 0;
        if (rr.isMember("max_steps") && (rr["max_steps"].isInt64() || rr["max_steps"].isUInt64() || rr["max_steps"].isInt() || rr["max_steps"].isUInt())) {
          req_limit = std::max<int64_t>(0, rr["max_steps"].asInt64());
        }
        const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, wf_steps_remaining) : wf_steps_remaining;
        rr["max_steps"] = (Json::Int64)std::max<int64_t>(0, eff);
        request_body = json_stringify_compact(rr);
      }
    }
    if (wf_limits.max_elapsed_ms_total > 0) {
      if (wf_elapsed_ms_remaining <= 0) {
        workflow_budget_exceeded_cancel("max_elapsed_ms_total");
        return;
      }
      if (rr.isObject()) {
        int64_t req_limit = 0;
        if (rr.isMember("timeout_ms") && (rr["timeout_ms"].isInt64() || rr["timeout_ms"].isUInt64() || rr["timeout_ms"].isInt() || rr["timeout_ms"].isUInt())) {
          req_limit = std::max<int64_t>(0, rr["timeout_ms"].asInt64());
        }
        const int64_t eff = (req_limit > 0) ? std::min<int64_t>(req_limit, wf_elapsed_ms_remaining) : wf_elapsed_ms_remaining;
        rr["timeout_ms"] = (Json::Int64)std::max<int64_t>(0, eff);
        request_body = json_stringify_compact(rr);
      }
    }
    if (wf_limits.max_total_tokens > 0) {
      if (wf_total_tokens_remaining <= 0) {
        workflow_budget_exceeded_cancel("max_total_tokens");
        return;
      }
    }
    out = run_request_to_json_internal_cancellable(
      cfg,
      ocfg,
      db_,
      tool_ext_or_null_,
      sessions_root_dir_,
      request_body,
      nullptr,
      workflow_run_should_cancel,
      &cancel_ctx
    );
  }

  // Ensure deterministic tasks (and budget cancellation results) have basic telemetry fields.
  if (out.isObject()) {
    const auto elapsed_ms =
      (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_steady).count();
    if (!out.isMember("elapsed_ms") || !(out["elapsed_ms"].isInt64() || out["elapsed_ms"].isUInt64() || out["elapsed_ms"].isInt() || out["elapsed_ms"].isUInt())) {
      out["elapsed_ms"] = (Json::Int64)std::max<int64_t>(0, elapsed_ms);
    }
    if (!out.isMember("tool_calls_total") || !(out["tool_calls_total"].isInt64() || out["tool_calls_total"].isUInt64() || out["tool_calls_total"].isInt() || out["tool_calls_total"].isUInt())) {
      out["tool_calls_total"] = (Json::Int64)0;
    }
    if (!out.isMember("steps_executed") || !(out["steps_executed"].isInt64() || out["steps_executed"].isUInt64() || out["steps_executed"].isInt() || out["steps_executed"].isUInt())) {
      out["steps_executed"] = (Json::Int64)0;
    }
    if (!out.isMember("prompt_tokens") || !(out["prompt_tokens"].isInt64() || out["prompt_tokens"].isUInt64() || out["prompt_tokens"].isInt() || out["prompt_tokens"].isUInt())) {
      out["prompt_tokens"] = (Json::Int64)0;
    }
    if (!out.isMember("completion_tokens") || !(out["completion_tokens"].isInt64() || out["completion_tokens"].isUInt64() || out["completion_tokens"].isInt() || out["completion_tokens"].isUInt())) {
      out["completion_tokens"] = (Json::Int64)0;
    }
    if (!out.isMember("total_tokens") || !(out["total_tokens"].isInt64() || out["total_tokens"].isUInt64() || out["total_tokens"].isInt() || out["total_tokens"].isUInt())) {
      out["total_tokens"] = (Json::Int64)0;
    }
  }

  std::string expect_err;
  const bool expect_ok = apply_expectations(task.expect_json, out, &expect_err);
  const bool run_ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
  const bool run_cancelled =
    out.isObject() && out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool();

  AgentDb::WorkflowTaskRow upd = task;
  upd.updated_unix_ms = now;
  upd.finished_unix_ms = now;
  upd.result_json = json_stringify_compact(out);

  // Retry-safe cumulative cost accounting (monotonic across attempts).
  {
    int64_t tool_calls_attempt = 0;
    if (out.isObject() && out.isMember("tool_calls_total") && (out["tool_calls_total"].isInt64() || out["tool_calls_total"].isUInt64() || out["tool_calls_total"].isInt() || out["tool_calls_total"].isUInt())) {
      tool_calls_attempt = std::max<int64_t>(0, out["tool_calls_total"].asInt64());
    }
    int64_t steps_attempt = 0;
    if (out.isObject() && out.isMember("steps_executed") && (out["steps_executed"].isInt64() || out["steps_executed"].isUInt64() || out["steps_executed"].isInt() || out["steps_executed"].isUInt())) {
      steps_attempt = std::max<int64_t>(0, out["steps_executed"].asInt64());
    }
    int64_t elapsed_attempt = 0;
    if (out.isObject() && out.isMember("elapsed_ms") && (out["elapsed_ms"].isInt64() || out["elapsed_ms"].isUInt64() || out["elapsed_ms"].isInt() || out["elapsed_ms"].isUInt())) {
      elapsed_attempt = std::max<int64_t>(0, out["elapsed_ms"].asInt64());
    }
    int64_t prompt_tokens_attempt = 0;
    if (out.isObject() && out.isMember("prompt_tokens") && (out["prompt_tokens"].isInt64() || out["prompt_tokens"].isUInt64() || out["prompt_tokens"].isInt() || out["prompt_tokens"].isUInt())) {
      prompt_tokens_attempt = std::max<int64_t>(0, out["prompt_tokens"].asInt64());
    }
    int64_t completion_tokens_attempt = 0;
    if (out.isObject() && out.isMember("completion_tokens") && (out["completion_tokens"].isInt64() || out["completion_tokens"].isUInt64() || out["completion_tokens"].isInt() || out["completion_tokens"].isUInt())) {
      completion_tokens_attempt = std::max<int64_t>(0, out["completion_tokens"].asInt64());
    }
    int64_t total_tokens_attempt = 0;
    if (out.isObject() && out.isMember("total_tokens") && (out["total_tokens"].isInt64() || out["total_tokens"].isUInt64() || out["total_tokens"].isInt() || out["total_tokens"].isUInt())) {
      total_tokens_attempt = std::max<int64_t>(0, out["total_tokens"].asInt64());
    } else if (prompt_tokens_attempt > 0 || completion_tokens_attempt > 0) {
      total_tokens_attempt = prompt_tokens_attempt + completion_tokens_attempt;
    }
    upd.tool_calls_total_cum = saturating_add_i64(std::max<int64_t>(0, task.tool_calls_total_cum), tool_calls_attempt);
    upd.steps_executed_cum = saturating_add_i64(std::max<int64_t>(0, task.steps_executed_cum), steps_attempt);
    upd.elapsed_ms_cum = saturating_add_i64(std::max<int64_t>(0, task.elapsed_ms_cum), elapsed_attempt);
    upd.prompt_tokens_cum = saturating_add_i64(std::max<int64_t>(0, task.prompt_tokens_cum), prompt_tokens_attempt);
    upd.completion_tokens_cum = saturating_add_i64(std::max<int64_t>(0, task.completion_tokens_cum), completion_tokens_attempt);
    upd.total_tokens_cum = saturating_add_i64(std::max<int64_t>(0, task.total_tokens_cum), total_tokens_attempt);
  }

  if (run_cancelled) {
    upd.status = "cancelled";
    upd.ready_unix_ms = 0;
    upd.error =
      out.isObject() && out.isMember("error") && out["error"].isString() && !out["error"].asString().empty()
      ? out["error"].asString()
      : (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled");
    (void)db_->upsert_workflow_task(upd, nullptr);

    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["reason"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline_exceeded" : "cancel_requested";
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }

    // Ensure the workflow is marked cancelled when a running task cooperatively cancels.
    AgentDb::WorkflowRow wcur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &wcur, &werr)) {
      if (!wcur.cancel_requested || (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded && wcur.error != "deadline exceeded")) {
        wcur.cancel_requested = true;
        wcur.updated_unix_ms = now;
        if (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded) wcur.error = "deadline exceeded";
        else if (wcur.error.empty()) wcur.error = "cancelled";
        (void)db_->upsert_workflow(wcur, nullptr);
      }
    }

    maybe_finalize_workflow(wf.workflow_id);
    return;
  }

  if (run_ok && expect_ok) {
    upd.status = "done";
    upd.error.clear();
    upd.ready_unix_ms = 0;
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["finished_unix_ms"] = (Json::Int64)upd.finished_unix_ms;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    maybe_finalize_workflow(wf.workflow_id);
    return;
  }

  const bool can_retry = upd.attempt < std::max(1, upd.max_attempts);
  upd.error = !expect_ok ? expect_err : (out.isObject() && out.isMember("error") && out["error"].isString() ? out["error"].asString() : "error");

  bool wf_cancel_requested = wf_latest.cancel_requested;
  {
    AgentDb::WorkflowRow cur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &cur, &werr)) {
      wf_cancel_requested = cur.cancel_requested;
    }
  }

  if (can_retry && !wf_cancel_requested) {
    upd.status = "queued";
    int64_t delay_ms = retry_backoff_ms(upd.attempt);
    if (out.isObject() && out.isMember("retryable") && out["retryable"].isBool() && out["retryable"].asBool() &&
        out.isMember("retry_in_ms") && (out["retry_in_ms"].isInt64() || out["retry_in_ms"].isUInt64() || out["retry_in_ms"].isInt())) {
      delay_ms = out["retry_in_ms"].asInt64();
      if (delay_ms < 0) delay_ms = 0;
      if (delay_ms > 60 * 1000) delay_ms = 60 * 1000;
    }
    upd.ready_unix_ms = now + delay_ms;
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["ready_unix_ms"] = (Json::Int64)upd.ready_unix_ms;
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    return;
  }

  upd.status = "error";
  upd.ready_unix_ms = 0;
  (void)db_->upsert_workflow_task(upd, nullptr);
  {
    Json::Value d(Json::objectValue);
    d["workflow_id"] = wf.workflow_id;
    d["task_id"] = upd.task_id;
    d["status"] = upd.status;
    d["attempt"] = upd.attempt;
    d["max_attempts"] = upd.max_attempts;
    if (!upd.error.empty()) d["error"] = upd.error;
    d["ts_unix_ms"] = (Json::Int64)now;
    insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
  }
  maybe_finalize_workflow(wf.workflow_id);
}

void WorkflowEngine::maybe_finalize_workflow(const std::string& workflow_id) {
  if (workflow_id.empty()) return;
  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_->get_workflow(workflow_id, &wf, &err)) return;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  if (!db_->list_workflow_tasks(workflow_id, &tasks, &err)) return;

  bool any_running = false;
  bool any_queued = false;
  bool any_hard_error = false;
  std::string first_error;
  bool all_terminal = true;
  for (const auto& t : tasks) {
    if (t.status == "running") any_running = true;
    if (t.status == "queued") any_queued = true;
    if (t.status == "error" && !t.allow_error) {
      any_hard_error = true;
      if (first_error.empty()) first_error = t.error;
    }
    const bool terminal = workflow_is_terminal_status(t.status);
    if (!terminal) all_terminal = false;
  }

  std::string new_status = wf.status;
  std::string wf_error = wf.error;

  if (any_hard_error) {
    new_status = "error";
    if (wf_error.empty()) wf_error = first_error.empty() ? "workflow task failed" : first_error;
  } else if (wf.cancel_requested) {
    if (all_terminal) {
      new_status = "cancelled";
      if (wf_error.empty()) wf_error = "cancelled";
    } else {
      new_status = "running";
    }
  } else if (all_terminal && !tasks.empty()) {
    new_status = "done";
    wf_error.clear();
  } else if (any_running) {
    new_status = "running";
  } else if (any_queued) {
    new_status = "queued";
  }

  // Persist final aggregation when terminal.
  std::string result_json;
  if (new_status == "done" || new_status == "error" || new_status == "cancelled") {
    Json::Value r(Json::objectValue);
    r["workflow_id"] = workflow_id;
    r["ok"] = (new_status == "done");
    r["status"] = new_status;
    if (!wf.trace_id.empty()) r["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) r["session_id"] = wf.session_id;
    if (!wf_error.empty()) r["error"] = wf_error;
    Json::Value arr(Json::arrayValue);
    Json::Value by_task(Json::objectValue);
    for (const auto& t : tasks) {
      Json::Value row(Json::objectValue);
      row["task_id"] = t.task_id;
      row["status"] = t.status;
      row["attempt"] = t.attempt;
      row["max_attempts"] = t.max_attempts;
      row["tool_calls_total_cum"] = (Json::Int64)std::max<int64_t>(0, t.tool_calls_total_cum);
      row["steps_executed_cum"] = (Json::Int64)std::max<int64_t>(0, t.steps_executed_cum);
      row["elapsed_ms_cum"] = (Json::Int64)std::max<int64_t>(0, t.elapsed_ms_cum);
      row["ready_unix_ms"] = (Json::Int64)t.ready_unix_ms;
      row["started_unix_ms"] = (Json::Int64)t.started_unix_ms;
      row["finished_unix_ms"] = (Json::Int64)t.finished_unix_ms;
      if (!t.error.empty()) row["error"] = t.error;
      if (!t.depends_on_json.empty()) {
        Json::Value deps;
        std::string derr;
        if (json_parse_any_value(t.depends_on_json, &deps, &derr) && deps.isArray()) row["depends_on"] = deps;
      }
      arr.append(row);

      if (!t.result_json.empty()) {
        Json::Value rr;
        std::string rerr;
        if (json_parse_any_value(t.result_json, &rr, &rerr)) {
          by_task[t.task_id] = rr;
        }
      }
    }
    r["tasks"] = arr;
    r["results_by_task"] = by_task;
    result_json = json_stringify_compact(r);
  }

  if (new_status != wf.status || wf_error != wf.error || (!result_json.empty() && result_json != wf.result_json)) {
    const std::string prev = wf.status;
    wf.status = new_status;
    wf.updated_unix_ms = unix_ms_now();
    wf.error = wf_error;
    if (!result_json.empty()) wf.result_json = result_json;
    (void)db_->upsert_workflow(wf, nullptr);

    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      d["prev_status"] = prev;
      d["status"] = new_status;
      d["cancel_requested"] = wf.cancel_requested;
      if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
      if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
      if (!wf.error.empty()) d["error"] = wf.error;
      d["ts_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
      insert_workflow_event_best_effort(db_, workflow_id, "", "workflow_status", wf.updated_unix_ms, d);
    }

    if (new_status == "done" || new_status == "error" || new_status == "cancelled") {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      d["ok"] = (new_status == "done");
      d["status"] = new_status;
      if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
      if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
      if (!wf.error.empty()) d["error"] = wf.error;
      d["result_json_present"] = !wf.result_json.empty();
      d["ts_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
      insert_workflow_event_best_effort(db_, workflow_id, "", "workflow_done", wf.updated_unix_ms, d);
    }
  }
}

}  // namespace agentd
