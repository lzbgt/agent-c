#include "workflow_run_budget_clamp.h"

#include <algorithm>
#include <cstdint>

namespace agentd {
namespace workflow_engine_internal {
namespace {

bool json_i64_best_effort(const Json::Value& v, int64_t* out) {
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

void clamp_or_set_i64(Json::Value* request, const char* key, int64_t cap) {
  if (!request || !request->isObject() || !key || cap <= 0) return;
  int64_t cur = 0;
  if (request->isMember(key) && json_i64_best_effort((*request)[key], &cur) && cur > 0) {
    (*request)[key] = (Json::Int64)std::min<int64_t>(cur, cap);
  } else {
    (*request)[key] = (Json::Int64)cap;
  }
}

void clamp_completion_token_cap(Json::Value* request, int64_t cap) {
  if (!request || !request->isObject() || cap <= 0) return;

  int64_t cur = 0;
  const bool has_completion =
    request->isMember("max_completion_tokens") &&
    json_i64_best_effort((*request)["max_completion_tokens"], &cur);
  const bool has_legacy =
    request->isMember("max_tokens") &&
    json_i64_best_effort((*request)["max_tokens"], &cur);

  if (has_completion) {
    clamp_or_set_i64(request, "max_completion_tokens", cap);
    if (has_legacy) clamp_or_set_i64(request, "max_tokens", cap);
    return;
  }
  if (has_legacy) {
    clamp_or_set_i64(request, "max_tokens", cap);
    return;
  }
  (*request)["max_completion_tokens"] = (Json::Int64)cap;
}

bool check_remaining(int64_t limit, int64_t remaining, const char* exceeded, std::string* out_exceeded) {
  if (limit <= 0) return true;
  if (remaining > 0) return true;
  if (out_exceeded) *out_exceeded = exceeded ? exceeded : "";
  return false;
}

}  // namespace

bool workflow_apply_run_budget_clamps(
  Json::Value* request,
  const WorkflowRunBudgetClampInput& input,
  std::string* out_exceeded
) {
  if (out_exceeded) out_exceeded->clear();

  if (!check_remaining(input.limits.max_tool_calls_total, input.tool_calls_remaining, "max_tool_calls_total", out_exceeded)) return false;
  if (!check_remaining(input.limits.max_steps_total, input.steps_remaining, "max_steps_total", out_exceeded)) return false;
  if (!check_remaining(input.limits.max_elapsed_ms_total, input.elapsed_ms_remaining, "max_elapsed_ms_total", out_exceeded)) return false;
  if (!check_remaining(input.limits.max_total_tokens, input.total_tokens_remaining, "max_total_tokens", out_exceeded)) return false;

  if (!request || !request->isObject()) return true;

  if (input.limits.max_tool_calls_total > 0) {
    clamp_or_set_i64(request, "max_tool_calls_total", std::max<int64_t>(0, input.tool_calls_remaining));
  }
  if (input.limits.max_steps_total > 0) {
    clamp_or_set_i64(request, "max_steps", std::max<int64_t>(0, input.steps_remaining));
  }
  if (input.limits.max_elapsed_ms_total > 0) {
    clamp_or_set_i64(request, "timeout_ms", std::max<int64_t>(0, input.elapsed_ms_remaining));
  }
  if (input.limits.max_total_tokens > 0) {
    clamp_completion_token_cap(request, std::max<int64_t>(0, input.total_tokens_remaining));
  }

  return true;
}

}  // namespace workflow_engine_internal
}  // namespace agentd
