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
  if (kind == "edge_invoke") {
    // Edge tasks are often multi-poll loops (QUEUED/RUNNING), so charge higher to reduce latency
    // variance under mixed workloads.
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

  return clamp_i64(cost, 1, max_cost);
}

}  // namespace agentd

