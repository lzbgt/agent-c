#include "workflow_budget.h"
#include "string_util.h"
#include <algorithm>
#include <unordered_set>
namespace agentd {
using Amounts = AgentDb::WorkflowBudgetAmounts;
const char *workflow_budget_dimension(int i) {
  static const char *names[] = {"max_tool_calls_total", "max_steps_total",
                                "max_elapsed_ms_total", "max_total_tokens"};
  return i >= 0 && i < 4 ? names[i] : "budget";
}
Amounts
workflow_budget_limits(const workflow_engine_internal::WorkflowLimits &l) {
  return {l.max_tool_calls_total, l.max_steps_total, l.max_elapsed_ms_total,
          l.max_total_tokens};
}
Json::Value workflow_budget_amounts_json(const Amounts &a) {
  Json::Value out(Json::objectValue);
  for (int i = 0; i < 4; i++)
    out[workflow_budget_dimension(i)] = Json::Int64(a[i]);
  return out;
}
Amounts workflow_budget_demand(const Json::Value &input,
                               const DaemonConfig &cfg, const Amounts &limits) {
  const Json::Value request =
      input.isObject() ? input : Json::Value(Json::objectValue);
  const auto value = [](const Json::Value &obj, const char *key,
                        int64_t fallback) {
    return obj.isObject() && obj[key].isInt64() && obj[key].asInt64() > 0
               ? obj[key].asInt64()
               : fallback;
  };
  const auto kind =
      request["kind"].isString() ? request["kind"].asString() : std::string();
  static const std::unordered_set<std::string> deterministic = {
      "delay",
      "aggregate",
      "edge_wait_sensor",
      "edge_invoke",
      "http_json",
      "host_tool",
      "avm_capsule",
      "experience_record",
      "memory_put",
      "memory_consolidate",
      "memory_search",
      "memory_timeline",
      "memory_structured_query",
      "memory_correlate",
      "memory_query"};
  const bool model = deterministic.count(kind) == 0;
  const bool pure =
      kind == "delay" || kind == "aggregate" || kind == "edge_wait_sensor";
  Amounts demand{};
  if (model) {
    const auto tools = lower_copy(trim_copy(
        request["tools"].isString() ? request["tools"].asString() : cfg.tools));
    demand[0] = tools == "none" && kind != "delegate" && kind != "agentd_call"
                    ? 0
                    : value(request, "max_tool_calls_total",
                            cfg.max_tool_calls_total_default
                                ? cfg.max_tool_calls_total_default
                                : limits[0]);
    demand[1] =
        value(request, "max_steps",
              cfg.max_steps_default ? cfg.max_steps_default : limits[1]);
    demand[3] = limits[3]; // Prompt/total usage has no certified
                           // provider-independent estimator.
  } else if (!pure) {
    demand[0] = 1;
    demand[1] = 1;
  }
  if (kind == "http_json" && request["http_json"].isObject() &&
      request["http_json"]["effect"].isObject() &&
      request["http_json"]["effect"]["reconcile_url"].isString() &&
      !request["http_json"]["effect"]["reconcile_url"].asString().empty())
    demand[0] = 2;
  const auto &timed = kind == "host_tool"     ? request["host_tool"]
                      : kind == "http_json"     ? request["http_json"]
                      : kind == "agentd_call" ? request["agentd_call"]
                                              : request;
  demand[2] =
      kind == "delay"
          ? std::min<int64_t>(
                600000, std::max<int64_t>(0, value(request, "delay_ms", 0))) +
                1
          : value(timed, "timeout_ms", std::max<int64_t>(1, cfg.timeout_ms));
  for (int i = 0; i < 4; i++)
    demand[i] = limits[i] > 0 ? std::min(limits[i], demand[i]) : 0;
  return demand;
}
} // namespace agentd
