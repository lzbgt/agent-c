#pragma once
#include "agent_db.h"
#include <json/json.h>
#include <functional>
#include <map>

namespace agentd {
struct WorkflowEffectContext {
  AgentDb* db = nullptr;
  std::string workflow_id;
  std::string task_id;
  std::function<bool()> should_cancel = {};
  int64_t max_network_calls = 0; // 0 means no workflow grant limit.
  std::function<bool()> dispatch_capacity_available = {};
};
using EffectDispatch = std::function<Json::Value(const std::map<std::string,std::string>&)>;
using EffectReconcile = std::function<Json::Value(const Json::Value&)>;
bool workflow_effect_validate_http(const Json::Value& spec, std::string* error);
Json::Value workflow_effect_execute(const WorkflowEffectContext& context, const Json::Value& spec,
                                    const EffectDispatch& dispatch, const EffectReconcile& reconcile);
struct GuardedEffectResult { Json::Value result; bool terminal = false; };
using GuardedEffectDispatch = std::function<GuardedEffectResult()>;
Json::Value workflow_effect_execute_guarded(const WorkflowEffectContext& context,
    const Json::Value& request, const GuardedEffectDispatch& dispatch,
    const std::function<Json::Value()>& authorize_dispatch = {});
}  // namespace agentd
