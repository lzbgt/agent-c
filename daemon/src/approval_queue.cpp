#include "approval_queue.h"

#include "agent_db.h"
#include "edge_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "policy_hooks.h"
#include "string_util.h"

#include "agent_sha256.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace agentd {
namespace {

static std::string sha256_hex_or_empty(const std::string& s) {
  if (s.empty()) return std::string();
  char hex[65];
  agent_sha256_hex_of_bytes(s.data(), s.size(), hex);
  return std::string(hex);
}

static Json::Value roles_to_json_array(const std::vector<std::string>& roles) {
  Json::Value arr(Json::arrayValue);
  for (const auto& r : roles) {
    if (!r.empty()) arr.append(r);
  }
  return arr;
}

static Json::Value role_constraints_from_row(const AgentDb::ApprovalRequestRow& req, const ApprovalGateCtx& gate) {
  if (!req.role_constraints_json.empty()) {
    Json::Value v;
    std::string err;
    if (json_parse_any(req.role_constraints_json, &v, &err) && v.isArray()) return v;
  }
  return roles_to_json_array(gate.roles);
}

static Json::Value approval_event_base(const AgentDb::ApprovalRequestRow& req, const ApprovalGateCtx& gate) {
  Json::Value d(Json::objectValue);
  if (!req.approval_id.empty()) d["approval_id"] = req.approval_id;
  const std::string trace_id = !req.trace_id.empty() ? req.trace_id : gate.trace_id;
  if (!trace_id.empty()) d["trace_id"] = trace_id;
  const int64_t run_id = req.run_id > 0 ? req.run_id : gate.run_id;
  if (run_id > 0) d["run_id"] = (Json::Int64)run_id;
  if (!req.session_id.empty()) d["session_id"] = req.session_id;
  else if (!gate.session_id.empty()) d["session_id"] = gate.session_id;
  if (!req.job_id.empty()) d["job_id"] = req.job_id;
  else if (!gate.job_id.empty()) d["job_id"] = gate.job_id;
  if (!req.team_id.empty()) d["team_id"] = req.team_id;
  else if (!gate.team_id.empty()) d["team_id"] = gate.team_id;
  if (!req.tool_name.empty()) d["tool_name"] = req.tool_name;
  if (!req.tool_call_id.empty()) d["tool_call_id"] = req.tool_call_id;
  if (!req.tool_args_hash.empty()) d["tool_args_hash"] = req.tool_args_hash;
  if (!req.status.empty()) d["status"] = req.status;
  if (req.required_approvals > 0) d["required_approvals"] = req.required_approvals;
  Json::Value roles = role_constraints_from_row(req, gate);
  if (roles.isArray() && !roles.empty()) d["role_constraints"] = roles;
  if (req.created_unix_ms > 0) d["created_unix_ms"] = (Json::Int64)req.created_unix_ms;
  if (req.expires_unix_ms > 0) d["expires_unix_ms"] = (Json::Int64)req.expires_unix_ms;
  if (!req.decision_reason.empty()) d["decision_reason"] = req.decision_reason;
  return d;
}

static void emit_approval_request(ApprovalGateCtx* gate, const AgentDb::ApprovalRequestRow& req) {
  if (!gate || !gate->hook) return;
  Json::Value d = approval_event_base(req, *gate);
  if (!d.isMember("status")) d["status"] = "pending";
  policy_emit_custom_event(gate->hook, "approval_request", d);
}

static void emit_approval_update(
  ApprovalGateCtx* gate,
  const AgentDb::ApprovalRequestRow& req,
  const AgentDb::ApprovalDecisionRow& decision,
  int approved,
  int required
) {
  if (!gate || !gate->hook) return;
  Json::Value d = approval_event_base(req, *gate);
  if (!decision.member_id.empty()) d["member_id"] = decision.member_id;
  if (!decision.decision.empty()) d["decision"] = decision.decision;
  if (decision.decision_unix_ms > 0) d["decision_unix_ms"] = (Json::Int64)decision.decision_unix_ms;
  if (!decision.note.empty()) d["note"] = decision.note;
  d["approved"] = approved;
  d["required_approvals"] = required;
  policy_emit_custom_event(gate->hook, "approval_update", d);
}

static void emit_approval_resolved(
  ApprovalGateCtx* gate,
  const AgentDb::ApprovalRequestRow& req,
  int approved,
  int required
) {
  if (!gate || !gate->hook) return;
  Json::Value d = approval_event_base(req, *gate);
  d["approved"] = approved;
  d["required_approvals"] = required;
  policy_emit_custom_event(gate->hook, "approval_resolved", d);
}

static std::string normalize_decision(const std::string& raw) {
  return lower_copy(trim_copy(raw));
}

static void count_decisions(
  const std::vector<AgentDb::ApprovalDecisionRow>& rows,
  int* out_approved,
  bool* out_denied
) {
  if (out_approved) *out_approved = 0;
  if (out_denied) *out_denied = false;
  std::unordered_set<std::string> approved_members;
  std::unordered_set<std::string> denied_members;
  for (const auto& row : rows) {
    const std::string d = normalize_decision(row.decision);
    if (d == "deny") {
      denied_members.insert(row.member_id);
    } else if (d == "approve") {
      approved_members.insert(row.member_id);
    }
  }
  if (out_approved) *out_approved = (int)approved_members.size();
  if (out_denied) *out_denied = !denied_members.empty();
}

static bool approval_tool_matches(const ApprovalGateCtx& gate, const std::string& tool_name) {
  if (tool_name.empty() || gate.toolset.empty()) return false;
  return gate.toolset.find(tool_name) != gate.toolset.end();
}

static AgentDb::ApprovalRequestRow build_request_row(
  ApprovalGateCtx* gate,
  const std::string& tool_name,
  const std::string& tool_call_id,
  const std::string& arguments_json
) {
  AgentDb::ApprovalRequestRow req;
  const std::string uuid = edge_make_uuidish_msg_id();
  req.approval_id = "approval_" + uuid;
  req.run_id = gate ? gate->run_id : 0;
  if (gate) {
    req.trace_id = gate->trace_id;
    req.session_id = gate->session_id;
    req.job_id = gate->job_id;
    req.team_id = gate->team_id;
  }
  req.tool_name = tool_name;
  req.tool_call_id = tool_call_id;
  req.tool_args_hash = sha256_hex_or_empty(arguments_json);
  req.required_approvals = gate ? gate->required : 1;
  if (gate && !gate->roles.empty()) {
    req.role_constraints_json = json_stringify(roles_to_json_array(gate->roles));
  }
  req.status = "pending";
  req.created_unix_ms = now_unix_ms();
  if (gate && gate->timeout_ms > 0) {
    req.expires_unix_ms = req.created_unix_ms + gate->timeout_ms;
  }
  return req;
}

}  // namespace

agent_status_t approval_gate_tool(
  ApprovalGateCtx* gate,
  const std::string& tool_name,
  const std::string& tool_call_id,
  const std::string& arguments_json
) {
  if (!gate) return AGENT_ERR_INVALID_ARGUMENT;
  if (!approval_tool_matches(*gate, tool_name)) return AGENT_OK;
  if (gate->required <= 0) return AGENT_OK;
  if (!gate->enforce && !gate->audit) return AGENT_OK;

  if (gate->audit && !gate->enforce) {
    AgentDb::ApprovalRequestRow req = build_request_row(gate, tool_name, tool_call_id, arguments_json);
    emit_approval_request(gate, req);
    req.status = "approved";
    req.decision_reason = "audit_auto_approve";
    emit_approval_resolved(gate, req, gate->required, gate->required);
    return AGENT_OK;
  }

  if (!gate->db || !gate->db->is_open()) {
    if (gate->hook) gate->hook->last_error = "approval gate requires db";
    return AGENT_ERR_INTERNAL;
  }

  AgentDb::ApprovalRequestRow req = build_request_row(gate, tool_name, tool_call_id, arguments_json);
  std::string db_err;
  if (!gate->db->insert_approval_request(req, &db_err)) {
    if (gate->hook) gate->hook->last_error = db_err.empty() ? "failed to insert approval request" : db_err;
    return AGENT_ERR_INTERNAL;
  }

  emit_approval_request(gate, req);

  int64_t last_decision_id = 0;
  const int64_t poll_ms = std::max<int64_t>(50, gate->poll_ms);

  for (;;) {
    if (!gate->job_id.empty() && job_is_cancel_requested(gate->job_id)) {
      req.status = "denied";
      req.decision_reason = "cancelled";
      (void)gate->db->update_approval_status(req.approval_id, req.status, req.decision_reason, nullptr);
      emit_approval_resolved(gate, req, 0, gate->required);
      if (gate->hook) gate->hook->last_error = "approval gate cancelled";
      return AGENT_ERR_CANCELLED;
    }

    AgentDb::ApprovalRequestRow cur;
    if (!gate->db->get_approval_request(req.approval_id, &cur, &db_err)) {
      if (gate->hook) gate->hook->last_error = db_err.empty() ? "approval request not found" : db_err;
      return AGENT_ERR_INTERNAL;
    }
    if (!cur.status.empty() && cur.status != "pending") {
      const std::string st = lower_copy(cur.status);
      if (st == "approved") {
        emit_approval_resolved(gate, cur, gate->required, gate->required);
        return AGENT_OK;
      }
      if (st == "denied" || st == "expired") {
        emit_approval_resolved(gate, cur, 0, gate->required);
        if (gate->hook && gate->hook->last_error.empty()) {
          gate->hook->last_error = "approval gate " + st;
        }
        return AGENT_ERR_LIMIT;
      }
    }

    std::vector<AgentDb::ApprovalDecisionRow> decisions;
    if (!gate->db->list_approval_decisions(req.approval_id, &decisions, &db_err)) {
      if (gate->hook) gate->hook->last_error = db_err.empty() ? "failed to list approval decisions" : db_err;
      return AGENT_ERR_INTERNAL;
    }

    int approved = 0;
    bool denied = false;
    count_decisions(decisions, &approved, &denied);

    for (const auto& d : decisions) {
      if (d.id <= last_decision_id) continue;
      emit_approval_update(gate, req, d, approved, gate->required);
      last_decision_id = std::max<int64_t>(last_decision_id, d.id);
    }

    const int64_t now_ms = now_unix_ms();
    if (denied) {
      req.status = "denied";
      req.decision_reason = "denied";
      (void)gate->db->update_approval_status(req.approval_id, req.status, req.decision_reason, nullptr);
      emit_approval_resolved(gate, req, approved, gate->required);
      if (gate->hook && gate->hook->last_error.empty()) {
        gate->hook->last_error = "approval denied";
      }
      return AGENT_ERR_LIMIT;
    }
    if (approved >= gate->required) {
      req.status = "approved";
      req.decision_reason = "approved";
      (void)gate->db->update_approval_status(req.approval_id, req.status, req.decision_reason, nullptr);
      emit_approval_resolved(gate, req, approved, gate->required);
      return AGENT_OK;
    }
    if (req.expires_unix_ms > 0 && now_ms >= req.expires_unix_ms) {
      req.status = "expired";
      req.decision_reason = "expired";
      (void)gate->db->update_approval_status(req.approval_id, req.status, req.decision_reason, nullptr);
      emit_approval_resolved(gate, req, approved, gate->required);
      if (gate->hook && gate->hook->last_error.empty()) {
        gate->hook->last_error = "approval expired";
      }
      return AGENT_ERR_LIMIT;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
}

}  // namespace agentd
