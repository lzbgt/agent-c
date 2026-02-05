#include "workflow_fairq_cost.h"

#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>

namespace agentd {

static int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool json_i64_best_effort(const Json::Value& v, int64_t* out) {
  if (!out) return false;
  if (v.isInt64()) {
    *out = v.asInt64();
    return true;
  }
  if (v.isUInt64()) {
    const uint64_t u = v.asUInt64();
    *out = (u > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)u;
    return true;
  }
  if (v.isInt()) {
    *out = (int64_t)v.asInt();
    return true;
  }
  if (v.isUInt()) {
    *out = (int64_t)v.asUInt();
    return true;
  }
  return false;
}

static bool json_bool_best_effort(const Json::Value& v, bool* out) {
  if (!out) return false;
  if (!v.isBool()) return false;
  *out = v.asBool();
  return true;
}

int64_t workflow_fairq_estimate_task_cost_simple_v1(
  const std::string& workflow_task_request_json,
  int64_t max_cost
) {
  max_cost = clamp_i64(max_cost, 1, 1024);

  Json::Value rr;
  std::string perr;
  if (!json_parse_object(workflow_task_request_json, &rr, &perr)) {
    return 1;
  }

  const std::string kind =
    rr.isMember("kind") && rr["kind"].isString() ? trim_copy(rr["kind"].asString()) : "";

  // Baseline costs by kind (rough heuristics).
  int64_t cost = 1;
  const bool looks_like_llm_run =
    kind.empty() &&
    rr.isMember("prompt") && rr["prompt"].isString() &&
    rr.isMember("model") && rr["model"].isString() && !trim_copy(rr["model"].asString()).empty();
  if (looks_like_llm_run) {
    // Default LLM runs are usually more expensive than deterministic tasks.
    cost = 2;
  }
  if (kind == "edge_invoke") {
    // Edge tasks are often multi-poll loops (QUEUED/RUNNING), so charge higher to reduce latency
    // variance under mixed workloads.
    cost = 8;
  } else if (kind == "agentd_call") {
    // Agent-to-agent collaboration is also commonly a poll loop (submit+poll until terminal).
    // Charge similar to edge_invoke to reduce tail latency when mixed with short deterministic tasks.
    cost = 8;
  } else if (kind == "delegate") {
    // Delegate can perform multiple sequential LLM calls inside a single workflow task.
    int64_t attempts = 0;
    if (rr.isMember("delegate") && rr["delegate"].isObject()) {
      const Json::Value a = rr["delegate"]["attempts"];
      if (a.isArray()) attempts = (int64_t)a.size();
    }
    // Worst-case cost scales with number of attempts.
    cost = std::max<int64_t>(2, 1 + attempts);
    cost = std::min<int64_t>(16, cost);
  } else if (kind == "aggregate") {
    cost = 1;
  } else if (kind == "memory_put" || kind == "memory_consolidate") {
    cost = 1;
  }

  // Best-effort: incorporate explicit request budgets when present.
  // These are hints for expected runtime/cost (not enforced by this function).
  int64_t max_tool_calls_total = 0;
  if (rr.isMember("max_tool_calls_total") && json_i64_best_effort(rr["max_tool_calls_total"], &max_tool_calls_total)) {
    if (max_tool_calls_total > 0) {
      cost += std::min<int64_t>(8, max_tool_calls_total / 10);
    }
  }
  int64_t max_steps = 0;
  if (rr.isMember("max_steps") && json_i64_best_effort(rr["max_steps"], &max_steps)) {
    if (max_steps > 0) {
      cost += std::min<int64_t>(4, max_steps / 50);
    }
  }

  // Prompt size is a strong predictor of latency and cost for LLM calls.
  if (rr.isMember("prompt") && rr["prompt"].isString()) {
    const int64_t n = (int64_t)rr["prompt"].asString().size();
    if (n > 0) {
      cost += std::min<int64_t>(8, n / 2000);
    }
  }

  // Streaming assistant mode tends to keep workers busy longer (SSE decoding + tool-loop event emission),
  // so charge a small bump to reduce tail latency under mixed workloads.
  if (rr.isMember("stream_assistant") && rr["stream_assistant"].isBool() && rr["stream_assistant"].asBool()) {
    cost += 1;
  }

  return clamp_i64(cost, 1, max_cost);
}

int64_t workflow_fairq_estimate_task_cost_telemetry_v1(
  const std::string& workflow_task_last_result_json,
  int64_t max_cost
) {
  max_cost = clamp_i64(max_cost, 1, 1024);
  if (workflow_task_last_result_json.empty()) return 0;

  Json::Value rr;
  std::string perr;
  if (!json_parse_object(workflow_task_last_result_json, &rr, &perr)) {
    return 0;
  }

  int64_t elapsed_ms = 0;
  if (rr.isMember("elapsed_ms") && json_i64_best_effort(rr["elapsed_ms"], &elapsed_ms)) {
    elapsed_ms = std::max<int64_t>(0, elapsed_ms);
  } else {
    elapsed_ms = 0;
  }

  int64_t total_tokens = 0;
  if (rr.isMember("total_tokens") && json_i64_best_effort(rr["total_tokens"], &total_tokens)) {
    total_tokens = std::max<int64_t>(0, total_tokens);
  } else {
    total_tokens = 0;
  }

  int64_t tool_calls_total = 0;
  if (rr.isMember("tool_calls_total") && json_i64_best_effort(rr["tool_calls_total"], &tool_calls_total)) {
    tool_calls_total = std::max<int64_t>(0, tool_calls_total);
  } else {
    tool_calls_total = 0;
  }

  int64_t steps_executed = 0;
  if (rr.isMember("steps_executed") && json_i64_best_effort(rr["steps_executed"], &steps_executed)) {
    steps_executed = std::max<int64_t>(0, steps_executed);
  } else {
    steps_executed = 0;
  }

  bool retryable = false;
  (void)(rr.isMember("retryable") && json_bool_best_effort(rr["retryable"], &retryable));

  int64_t retry_in_ms = 0;
  if (rr.isMember("retry_in_ms") && json_i64_best_effort(rr["retry_in_ms"], &retry_in_ms)) {
    retry_in_ms = std::max<int64_t>(0, retry_in_ms);
  } else {
    retry_in_ms = 0;
  }

  // Telemetry-derived cost units (bounded and intentionally coarse):
  // - tokens and elapsed are strong correlates of CPU/network saturation under load
  // - retryable is a hint that this task is in a poll loop; charge a small bump so poll loops don't
  //   dominate admission under mixed workloads
  //
  // NOTE: This is a fairness heuristic, not a billing system.
  int64_t cost = 1;
  if (total_tokens > 0) {
    // +1 per ~400 tokens, capped.
    cost += std::min<int64_t>(32, total_tokens / 400);
  }
  if (elapsed_ms > 0) {
    // +1 per ~500ms of work, capped.
    cost += std::min<int64_t>(32, elapsed_ms / 500);
  }
  if (tool_calls_total > 0) {
    cost += std::min<int64_t>(8, tool_calls_total / 5);
  }
  if (steps_executed > 0) {
    cost += std::min<int64_t>(8, steps_executed / 50);
  }
  if (retryable) {
    cost += 1;
    // Fast polls can increase scheduler/db load; add a small bump when retry interval is very short.
    if (retry_in_ms > 0 && retry_in_ms < 200) cost += 1;
  }

  return clamp_i64(cost, 1, max_cost);
}

}  // namespace agentd
