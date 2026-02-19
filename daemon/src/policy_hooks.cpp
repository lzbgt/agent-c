#include "policy_hooks.h"

#include "daemon_config.h"
#include "job_manager.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>

namespace agentd {
namespace {

static bool policy_list_has(const std::unordered_set<std::string>& s, const std::string& name) {
  if (s.empty() || name.empty()) return false;
  return s.find(name) != s.end();
}

static void policy_emit_cap_event(
  PolicyHookCtx* ctx,
  const char* field,
  size_t requested,
  size_t effective,
  bool enforced
) {
  if (!ctx || !field) return;
  if (requested == effective) return;
  Json::Value d(Json::objectValue);
  d["phase"] = "pre_run";
  d["mode"] = policy_mode_to_string(ctx->cfg.mode);
  d["action"] = "cap";
  d["enforced"] = enforced;
  d["field"] = field;
  d["requested"] = (Json::UInt64)requested;
  d["effective"] = (Json::UInt64)effective;
  policy_emit_event(ctx, d);
}

static void policy_emit_tool_event(
  PolicyHookCtx* ctx,
  const char* action,
  const std::string& reason,
  bool enforced,
  const std::string& tool_name
) {
  if (!ctx || !action) return;
  Json::Value d(Json::objectValue);
  d["phase"] = "tool_call";
  d["mode"] = policy_mode_to_string(ctx->cfg.mode);
  d["action"] = action;
  d["enforced"] = enforced;
  if (!reason.empty()) d["reason"] = reason;
  if (!tool_name.empty()) d["tool_name"] = tool_name;
  if (!ctx->last_tool_call_id.empty()) d["tool_call_id"] = ctx->last_tool_call_id;
  if (ctx->last_step >= 0) d["step"] = (Json::Int64)ctx->last_step;
  policy_emit_event(ctx, d);
}

static bool policy_active_mode(const PolicyConfig& cfg) {
  return cfg.mode != PolicyMode::Off;
}

}  // namespace

const char* policy_mode_to_string(PolicyMode mode) {
  switch (mode) {
    case PolicyMode::Audit:
      return "audit";
    case PolicyMode::Enforce:
      return "enforce";
    case PolicyMode::Off:
    default:
      return "off";
  }
}

bool policy_mode_from_string(const std::string& s, PolicyMode* out) {
  if (!out) return false;
  const std::string v = lower_copy(trim_copy(s));
  if (v == "off" || v.empty()) {
    *out = PolicyMode::Off;
    return true;
  }
  if (v == "audit") {
    *out = PolicyMode::Audit;
    return true;
  }
  if (v == "enforce") {
    *out = PolicyMode::Enforce;
    return true;
  }
  return false;
}

PolicyConfig policy_config_from_daemon(const DaemonConfig& cfg) {
  PolicyConfig out;
  PolicyMode mode = PolicyMode::Off;
  if (policy_mode_from_string(cfg.policy_mode, &mode)) {
    out.mode = mode;
  }
  out.tool_allowlist = cfg.policy_tool_allowlist;
  out.tool_denylist = cfg.policy_tool_denylist;
  out.max_steps = cfg.policy_max_steps;
  out.max_tool_calls_total = cfg.policy_max_tool_calls_total;
  out.max_tool_calls_per_tool = cfg.policy_max_tool_calls_per_tool;
  out.max_tool_call_args_chars = cfg.policy_max_tool_call_args_chars;
  out.max_tool_result_chars = cfg.policy_max_tool_result_chars;
  return out;
}

void policy_prepare(PolicyHookCtx* ctx, const PolicyConfig& cfg, const std::string& trace_id, const std::string& job_id) {
  if (!ctx) return;
  ctx->cfg = cfg;
  ctx->trace_id = trace_id;
  ctx->job_id = job_id;
  ctx->events = Json::Value(Json::arrayValue);
  ctx->allowset.clear();
  ctx->denyset.clear();
  for (const auto& s : cfg.tool_allowlist) {
    if (!s.empty()) ctx->allowset.insert(s);
  }
  for (const auto& s : cfg.tool_denylist) {
    if (!s.empty()) ctx->denyset.insert(s);
  }
  ctx->last_tool_call_id.clear();
  ctx->last_tool_name.clear();
  ctx->last_step = -1;
  ctx->last_error.clear();
}

void policy_emit_event(PolicyHookCtx* ctx, const Json::Value& data) {
  if (!ctx) return;
  Json::Value ev(Json::objectValue);
  ev["type"] = "policy_decision";
  if (!ctx->trace_id.empty()) ev["trace_id"] = ctx->trace_id;
  ev["data"] = data;
  ctx->events.append(ev);

  if (!ctx->job_id.empty()) {
    job_append_event(ctx->job_id, "policy_decision", json_stringify(data));
  }
}

void policy_emit_start(PolicyHookCtx* ctx) {
  if (!ctx || !policy_active_mode(ctx->cfg)) return;
  Json::Value d(Json::objectValue);
  d["phase"] = "pre_run";
  d["mode"] = policy_mode_to_string(ctx->cfg.mode);
  d["action"] = "start";
  d["allowlist_count"] = (Json::UInt64)ctx->cfg.tool_allowlist.size();
  d["denylist_count"] = (Json::UInt64)ctx->cfg.tool_denylist.size();
  policy_emit_event(ctx, d);
}

void policy_emit_complete(PolicyHookCtx* ctx, bool ok) {
  if (!ctx || !policy_active_mode(ctx->cfg)) return;
  Json::Value d(Json::objectValue);
  d["phase"] = "post_run";
  d["mode"] = policy_mode_to_string(ctx->cfg.mode);
  d["action"] = "complete";
  d["ok"] = ok;
  policy_emit_event(ctx, d);
}

void policy_apply_budget_caps(
  PolicyHookCtx* ctx,
  size_t* max_steps,
  size_t* max_tool_calls_total,
  size_t* max_tool_calls_per_tool,
  size_t* max_tool_call_args_chars,
  size_t* max_tool_result_chars
) {
  if (!ctx || !policy_active_mode(ctx->cfg)) return;
  auto apply_cap = [&](const char* field, size_t cap, size_t* value) {
    if (!value || cap == 0) return;
    const size_t requested = *value;
    const size_t effective = (requested == 0) ? cap : std::min(requested, cap);
    if (effective != requested) {
      const bool enforce = ctx->cfg.mode == PolicyMode::Enforce;
      if (enforce) *value = effective;
      policy_emit_cap_event(ctx, field, requested, effective, enforce);
    }
  };
  apply_cap("max_steps", ctx->cfg.max_steps, max_steps);
  apply_cap("max_tool_calls_total", ctx->cfg.max_tool_calls_total, max_tool_calls_total);
  apply_cap("max_tool_calls_per_tool", ctx->cfg.max_tool_calls_per_tool, max_tool_calls_per_tool);
  apply_cap("max_tool_call_args_chars", ctx->cfg.max_tool_call_args_chars, max_tool_call_args_chars);
  apply_cap("max_tool_result_chars", ctx->cfg.max_tool_result_chars, max_tool_result_chars);
}

void policy_on_tool_loop_event(void* vctx, const char* type, const char* data_json) {
  if (!vctx || !type) return;
  auto* ctx = static_cast<PolicyHookCtx*>(vctx);
  if (!policy_active_mode(ctx->cfg)) return;
  const std::string t(type);
  if (t != "tool_call") return;

  if (!data_json || !data_json[0]) return;
  Json::Value data;
  std::string perr;
  if (!json_parse_any(data_json, &data, &perr)) return;
  if (!data.isObject()) return;
  if (data.isMember("tool_name") && data["tool_name"].isString()) {
    ctx->last_tool_name = data["tool_name"].asString();
  }
  if (data.isMember("tool_call_id") && data["tool_call_id"].isString()) {
    ctx->last_tool_call_id = data["tool_call_id"].asString();
  }
  if (data.isMember("step") && (data["step"].isInt64() || data["step"].isUInt64() || data["step"].isInt())) {
    ctx->last_step = data["step"].asInt64();
  }
}

agent_status_t policy_tool_execute(
  void* vctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  if (!vctx) return AGENT_ERR_INVALID_ARGUMENT;
  auto* ctx = static_cast<PolicyToolExecutorCtx*>(vctx);
  if (!ctx->base.execute) return AGENT_ERR_INVALID_ARGUMENT;
  PolicyHookCtx* hook = ctx->hook;
  if (!hook || !policy_active_mode(hook->cfg)) {
    return ctx->base.execute(ctx->base.ctx, tool_name, arguments_json, out_result);
  }

  const std::string tool = tool_name ? tool_name : "";
  const bool in_deny = policy_list_has(hook->denyset, tool);
  const bool allowlist_active = !hook->allowset.empty();
  const bool in_allow = allowlist_active && policy_list_has(hook->allowset, tool);
  const bool deny = in_deny || (allowlist_active && !in_allow);

  if (deny) {
    const std::string reason = in_deny ? "tool_denylist" : "tool_not_allowlisted";
    const bool enforce = hook->cfg.mode == PolicyMode::Enforce;
    policy_emit_tool_event(hook, "deny", reason, enforce, tool);
    if (enforce) {
      hook->last_error = "policy denied tool: " + tool;
      return AGENT_ERR_LIMIT;
    }
  } else if (!hook->allowset.empty() || !hook->denyset.empty()) {
    const std::string reason = allowlist_active ? "tool_allowlist" : "tool_not_denied";
    policy_emit_tool_event(hook, "allow", reason, false, tool);
  }

  return ctx->base.execute(ctx->base.ctx, tool_name, arguments_json, out_result);
}

}  // namespace agentd
