#include "policy_hooks.h"
#include "approval_queue.h"

#include "daemon_config.h"
#include "job_manager.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <unordered_map>

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

static PolicyMode policy_mode_tighten(PolicyMode base, PolicyMode requested) {
  return (requested > base) ? requested : base;
}

static bool parse_string_array(const Json::Value& v, std::vector<std::string>* out, bool lower) {
  if (!out) return false;
  if (!v.isArray()) return false;
  out->clear();
  out->reserve(v.size());
  for (const auto& item : v) {
    if (!item.isString()) return false;
    std::string s = trim_copy(item.asString());
    if (s.empty()) continue;
    if (lower) s = lower_copy(s);
    out->push_back(std::move(s));
  }
  return true;
}

static bool parse_policy_approval_rule(const Json::Value& v, PolicyApprovalRule* out, std::string* err) {
  if (!out) return false;
  if (!v.isObject()) {
    if (err) *err = "policy_approval_rules item must be object";
    return false;
  }
  if (!v.isMember("tool_names")) {
    if (err) *err = "policy_approval_rules tool_names required";
    return false;
  }
  PolicyApprovalRule rule;
  if (!parse_string_array(v["tool_names"], &rule.tool_names, false) || rule.tool_names.empty()) {
    if (err) *err = "policy_approval_rules tool_names must be non-empty array";
    return false;
  }
  if (!v.isMember("min_approvals") || !v["min_approvals"].isInt()) {
    if (err) *err = "policy_approval_rules min_approvals required";
    return false;
  }
  const int min_approvals = (int)v["min_approvals"].asInt();
  if (min_approvals < 1) {
    if (err) *err = "policy_approval_rules min_approvals must be >= 1";
    return false;
  }
  rule.required = min_approvals;
  if (v.isMember("role_allowlist")) {
    if (!parse_string_array(v["role_allowlist"], &rule.roles, true)) {
      if (err) *err = "policy_approval_rules role_allowlist must be array of strings";
      return false;
    }
  }
  if (v.isMember("require_distinct_roles")) {
    if (!v["require_distinct_roles"].isBool()) {
      if (err) *err = "policy_approval_rules require_distinct_roles must be boolean";
      return false;
    }
    rule.require_distinct_roles = v["require_distinct_roles"].asBool();
  }
  if (v.isMember("timeout_ms")) {
    if (!v["timeout_ms"].isInt64()) {
      if (err) *err = "policy_approval_rules timeout_ms must be integer";
      return false;
    }
    const int64_t timeout_ms = v["timeout_ms"].asInt64();
    if (timeout_ms < 0) {
      if (err) *err = "policy_approval_rules timeout_ms must be >= 0";
      return false;
    }
    rule.timeout_ms = timeout_ms;
  }
  if (v.isMember("quorum_mode")) {
    if (!v["quorum_mode"].isString()) {
      if (err) *err = "policy_approval_rules quorum_mode must be string";
      return false;
    }
    const std::string q = lower_copy(trim_copy(v["quorum_mode"].asString()));
    if (q.empty() || q == "strict") {
      rule.best_effort = false;
    } else if (q == "best_effort" || q == "best-effort") {
      rule.best_effort = true;
    } else {
      if (err) *err = "policy_approval_rules quorum_mode invalid (expected strict|best_effort)";
      return false;
    }
  }
  *out = std::move(rule);
  return true;
}

static bool merge_role_allowlist(
  const std::vector<std::string>& base,
  const std::vector<std::string>& next,
  std::vector<std::string>* out,
  std::string* err
) {
  if (!out) return false;
  if (next.empty()) {
    *out = base;
    return true;
  }
  if (base.empty()) {
    *out = next;
    return true;
  }
  std::unordered_set<std::string> base_set(base.begin(), base.end());
  for (const auto& role : next) {
    if (base_set.find(role) == base_set.end()) {
      if (err) *err = "policy_approval_roles must be subset of daemon roles";
      return false;
    }
  }
  *out = next;
  return true;
}

static bool merge_policy_approval_rule(
  PolicyApprovalRule* base,
  const PolicyApprovalRule& next,
  std::string* err
) {
  if (!base) return false;
  base->required = std::max(base->required, next.required);
  base->require_distinct_roles = base->require_distinct_roles || next.require_distinct_roles;
  base->best_effort = base->best_effort && next.best_effort;
  if (next.timeout_ms > 0) {
    if (base->timeout_ms > 0) base->timeout_ms = std::min(base->timeout_ms, next.timeout_ms);
    else base->timeout_ms = next.timeout_ms;
  }
  std::vector<std::string> merged_roles;
  if (!merge_role_allowlist(base->roles, next.roles, &merged_roles, err)) return false;
  base->roles = std::move(merged_roles);
  return true;
}

static bool add_rule_for_tools(
  std::unordered_map<std::string, PolicyApprovalRule>* dst,
  const PolicyApprovalRule& rule,
  std::string* err
) {
  if (!dst) return false;
  for (const auto& tool : rule.tool_names) {
    const std::string name = trim_copy(tool);
    if (name.empty()) continue;
    auto it = dst->find(name);
    if (it == dst->end()) {
      PolicyApprovalRule entry = rule;
      entry.tool_names = {name};
      (*dst)[name] = std::move(entry);
    } else {
      if (!merge_policy_approval_rule(&it->second, rule, err)) return false;
    }
  }
  return true;
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
  out.approval_tools = cfg.policy_approval_tools;
  out.approval_required = std::max(0, cfg.policy_approval_required);
  out.approval_roles = cfg.policy_approval_roles;
  out.approval_timeout_ms = std::max<int64_t>(0, cfg.policy_approval_timeout_ms);
  out.approval_poll_ms = std::max<int64_t>(0, cfg.policy_approval_poll_ms);
  return out;
}

bool policy_apply_overrides_from_json(const Json::Value& args, PolicyConfig* cfg, std::string* out_error) {
  if (!cfg) {
    if (out_error) *out_error = "policy override missing config";
    return false;
  }
  if (!args.isObject()) return true;

  if (args.isMember("policy_mode")) {
    if (!args["policy_mode"].isString()) {
      if (out_error) *out_error = "policy_mode must be string";
      return false;
    }
    PolicyMode requested = PolicyMode::Off;
    if (!policy_mode_from_string(args["policy_mode"].asString(), &requested)) {
      if (out_error) *out_error = "invalid policy_mode";
      return false;
    }
    cfg->mode = policy_mode_tighten(cfg->mode, requested);
  }

  if (args.isMember("policy_approval_poll_ms")) {
    if (!args["policy_approval_poll_ms"].isInt64()) {
      if (out_error) *out_error = "policy_approval_poll_ms must be integer";
      return false;
    }
    const int64_t poll_ms = args["policy_approval_poll_ms"].asInt64();
    if (poll_ms < 0) {
      if (out_error) *out_error = "policy_approval_poll_ms must be >= 0";
      return false;
    }
    if (poll_ms > 0) {
      if (cfg->approval_poll_ms > 0) cfg->approval_poll_ms = std::min(cfg->approval_poll_ms, poll_ms);
      else cfg->approval_poll_ms = poll_ms;
    }
  }

  bool has_overrides = false;
  std::vector<PolicyApprovalRule> override_rules;

  if (args.isMember("policy_approval_rules")) {
    if (!args["policy_approval_rules"].isArray()) {
      if (out_error) *out_error = "policy_approval_rules must be array";
      return false;
    }
    for (const auto& item : args["policy_approval_rules"]) {
      PolicyApprovalRule rule;
      std::string err;
      if (!parse_policy_approval_rule(item, &rule, &err)) {
        if (out_error) *out_error = err.empty() ? "invalid policy_approval_rules" : err;
        return false;
      }
      override_rules.push_back(std::move(rule));
    }
    has_overrides = has_overrides || !override_rules.empty();
  }

  if (args.isMember("policy_approval_tools")) {
    if (!args["policy_approval_tools"].isArray()) {
      if (out_error) *out_error = "policy_approval_tools must be array";
      return false;
    }
    std::vector<std::string> tools;
    if (!parse_string_array(args["policy_approval_tools"], &tools, false)) {
      if (out_error) *out_error = "policy_approval_tools must be array of strings";
      return false;
    }
    if (!tools.empty()) {
      PolicyApprovalRule rule;
      rule.tool_names = std::move(tools);
      int required = cfg->approval_required;
      if (args.isMember("policy_approval_required")) {
        if (!args["policy_approval_required"].isInt()) {
          if (out_error) *out_error = "policy_approval_required must be integer";
          return false;
        }
        required = (int)args["policy_approval_required"].asInt();
      }
      if (required < 1) {
        if (out_error) *out_error = "policy_approval_required must be >= 1";
        return false;
      }
      rule.required = required;
      if (args.isMember("policy_approval_roles")) {
        if (!parse_string_array(args["policy_approval_roles"], &rule.roles, true)) {
          if (out_error) *out_error = "policy_approval_roles must be array of strings";
          return false;
        }
      } else {
        rule.roles = cfg->approval_roles;
      }
      int64_t timeout_ms = cfg->approval_timeout_ms;
      if (args.isMember("policy_approval_timeout_ms")) {
        if (!args["policy_approval_timeout_ms"].isInt64()) {
          if (out_error) *out_error = "policy_approval_timeout_ms must be integer";
          return false;
        }
        timeout_ms = args["policy_approval_timeout_ms"].asInt64();
      }
      if (timeout_ms < 0) {
        if (out_error) *out_error = "policy_approval_timeout_ms must be >= 0";
        return false;
      }
      rule.timeout_ms = timeout_ms;
      if (args.isMember("policy_approval_require_distinct_roles")) {
        if (!args["policy_approval_require_distinct_roles"].isBool()) {
          if (out_error) *out_error = "policy_approval_require_distinct_roles must be boolean";
          return false;
        }
        rule.require_distinct_roles = args["policy_approval_require_distinct_roles"].asBool();
      }
      if (args.isMember("policy_approval_quorum_mode")) {
        if (!args["policy_approval_quorum_mode"].isString()) {
          if (out_error) *out_error = "policy_approval_quorum_mode must be string";
          return false;
        }
        const std::string q = lower_copy(trim_copy(args["policy_approval_quorum_mode"].asString()));
        if (q == "best_effort" || q == "best-effort") rule.best_effort = true;
        else if (q.empty() || q == "strict") rule.best_effort = false;
        else {
          if (out_error) *out_error = "policy_approval_quorum_mode invalid (expected strict|best_effort)";
          return false;
        }
      }
      override_rules.push_back(std::move(rule));
      has_overrides = true;
    }
  }

  if (has_overrides) {
    std::unordered_map<std::string, PolicyApprovalRule> merged;
    if (!cfg->approval_rules.empty()) {
      for (const auto& rule : cfg->approval_rules) {
        std::string err;
        if (!add_rule_for_tools(&merged, rule, &err)) {
          if (out_error) *out_error = err.empty() ? "invalid policy_approval_rules" : err;
          return false;
        }
      }
    } else if (!cfg->approval_tools.empty()) {
      PolicyApprovalRule base;
      base.tool_names = cfg->approval_tools;
      base.required = std::max(1, cfg->approval_required);
      base.roles = cfg->approval_roles;
      base.timeout_ms = cfg->approval_timeout_ms;
      base.require_distinct_roles = false;
      base.best_effort = false;
      std::string err;
      if (!add_rule_for_tools(&merged, base, &err)) {
        if (out_error) *out_error = err.empty() ? "invalid policy_approval_tools" : err;
        return false;
      }
    }
    for (const auto& rule : override_rules) {
      std::string err;
      if (!add_rule_for_tools(&merged, rule, &err)) {
        if (out_error) *out_error = err.empty() ? "invalid policy_approval_rules" : err;
        return false;
      }
    }
    cfg->approval_rules.clear();
    cfg->approval_rules.reserve(merged.size());
    for (auto& item : merged) {
      PolicyApprovalRule r = item.second;
      r.tool_names = {item.first};
      cfg->approval_rules.push_back(std::move(r));
    }
    if (cfg->mode == PolicyMode::Off) {
      cfg->mode = PolicyMode::Enforce;
    }
  }

  return true;
}

void policy_prepare(
  PolicyHookCtx* ctx,
  const PolicyConfig& cfg,
  const std::string& trace_id,
  const std::string& job_id,
  const std::string& session_id
) {
  if (!ctx) return;
  ctx->cfg = cfg;
  ctx->trace_id = trace_id;
  ctx->session_id = session_id;
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

void policy_emit_custom_event(PolicyHookCtx* ctx, const char* type, const Json::Value& data) {
  if (!ctx || !type || !type[0]) return;
  Json::Value ev(Json::objectValue);
  ev["type"] = type;
  if (!ctx->trace_id.empty()) ev["trace_id"] = ctx->trace_id;
  ev["data"] = data;
  ctx->events.append(ev);

  if (!ctx->job_id.empty()) {
    job_append_event(ctx->job_id, type, json_stringify(data));
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

  if (ctx->approval_gate) {
    auto* gate = static_cast<ApprovalGateCtx*>(ctx->approval_gate);
    const std::string call_id = hook ? hook->last_tool_call_id : std::string();
    const std::string args = arguments_json ? std::string(arguments_json) : std::string();
    const agent_status_t gate_st = approval_gate_tool(gate, tool, call_id, args);
    if (gate_st != AGENT_OK) {
      if (hook && hook->last_error.empty()) {
        hook->last_error = "approval gate blocked tool: " + tool;
      }
      return gate_st;
    }
  }

  return ctx->base.execute(ctx->base.ctx, tool_name, arguments_json, out_result);
}

}  // namespace agentd
