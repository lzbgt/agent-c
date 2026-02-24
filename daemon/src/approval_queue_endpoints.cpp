#include "approval_queue_endpoints.h"

#include "cors.h"
#include "daemon_auth.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "agent_db.h"

#include <json/json.h>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace agentd {
namespace {

static Json::Value approval_row_to_json(const AgentDb::ApprovalRequestRow& row) {
  Json::Value out(Json::objectValue);
  out["approval_id"] = row.approval_id;
  if (row.run_id > 0) out["run_id"] = (Json::Int64)row.run_id;
  if (!row.trace_id.empty()) out["trace_id"] = row.trace_id;
  if (!row.session_id.empty()) out["session_id"] = row.session_id;
  if (!row.job_id.empty()) out["job_id"] = row.job_id;
  if (!row.team_id.empty()) out["team_id"] = row.team_id;
  if (!row.tool_name.empty()) out["tool_name"] = row.tool_name;
  if (!row.tool_call_id.empty()) out["tool_call_id"] = row.tool_call_id;
  if (!row.tool_args_hash.empty()) out["tool_args_hash"] = row.tool_args_hash;
  if (!row.status.empty()) out["status"] = row.status;
  if (row.required_approvals > 0) out["required_approvals"] = row.required_approvals;
  if (!row.role_constraints_json.empty()) {
    Json::Value rc;
    std::string err;
    if (json_parse_any(row.role_constraints_json, &rc, &err) && rc.isArray()) {
      out["role_constraints"] = rc;
    }
  }
  if (row.created_unix_ms > 0) out["created_unix_ms"] = (Json::Int64)row.created_unix_ms;
  if (row.expires_unix_ms > 0) out["expires_unix_ms"] = (Json::Int64)row.expires_unix_ms;
  if (!row.decision_reason.empty()) out["decision_reason"] = row.decision_reason;
  return out;
}

static Json::Value decision_row_to_json(const AgentDb::ApprovalDecisionRow& row) {
  Json::Value out(Json::objectValue);
  out["id"] = (Json::Int64)row.id;
  out["approval_id"] = row.approval_id;
  out["member_id"] = row.member_id;
  out["decision"] = row.decision;
  if (row.decision_unix_ms > 0) out["decision_unix_ms"] = (Json::Int64)row.decision_unix_ms;
  if (!row.note.empty()) out["note"] = row.note;
  return out;
}

static std::string approval_id_from_path(const std::string& path, bool* out_decisions) {
  if (out_decisions) *out_decisions = false;
  const std::string prefix = "/api/v1/approvals/";
  if (path.rfind(prefix, 0) != 0) return std::string();
  std::string suffix = path.substr(prefix.size());
  if (suffix.empty()) return std::string();
  const std::string decisions = "/decisions";
  if (suffix.size() > decisions.size() && suffix.rfind(decisions) == (suffix.size() - decisions.size())) {
    if (out_decisions) *out_decisions = true;
    suffix = suffix.substr(0, suffix.size() - decisions.size());
  }
  if (!suffix.empty() && suffix.back() == '/') suffix.pop_back();
  return suffix;
}

static bool parse_limit_param(const std::optional<std::string>& raw, size_t* out) {
  if (!out) return false;
  if (!raw || raw->empty()) return false;
  try {
    *out = (size_t)std::stoull(*raw);
    return true;
  } catch (...) {
    return false;
  }
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
    const std::string d = lower_copy(trim_copy(row.decision));
    if (d == "deny") {
      denied_members.insert(row.member_id);
    } else if (d == "approve") {
      approved_members.insert(row.member_id);
    }
  }
  if (out_approved) *out_approved = (int)approved_members.size();
  if (out_denied) *out_denied = !denied_members.empty();
}

}  // namespace

void handle_approvals_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }

  AgentDb::ApprovalListFilter filter;
  if (const auto v = query_get(req.query, "status")) filter.status = *v;
  if (const auto v = query_get(req.query, "team_id")) filter.team_id = *v;
  if (const auto v = query_get(req.query, "trace_id")) filter.trace_id = *v;
  if (const auto v = query_get(req.query, "job_id")) filter.job_id = *v;
  if (const auto v = query_get(req.query, "tool_name")) filter.tool_name = *v;
  if (const auto v = query_get(req.query, "run_id")) {
    try { filter.run_id = std::stoll(*v); } catch (...) { filter.run_id = 0; }
  }
  size_t limit = 0;
  if (parse_limit_param(query_get(req.query, "limit"), &limit)) {
    if (limit == 0) limit = 100;
    filter.limit = std::min<size_t>(limit, 500);
  }

  std::vector<AgentDb::ApprovalRequestRow> rows;
  std::string db_err;
  if (!db_or_null->list_approval_requests(filter, &rows, &db_err)) {
    resp->status = 500;
    resp->body = json_error_body(db_err.empty() ? "failed to list approvals" : db_err);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["limit"] = (Json::UInt64)(filter.limit > 0 ? filter.limit : 100);
  Json::Value arr(Json::arrayValue);
  for (const auto& row : rows) {
    arr.append(approval_row_to_json(row));
  }
  out["approvals"] = arr;
  resp->body = json_stringify(out);
}

void handle_approvals_prefix_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  bool wants_decisions = false;
  const std::string approval_id = approval_id_from_path(req.path, &wants_decisions);
  if (approval_id.empty() || !edge_id_is_safe(approval_id)) {
    resp->status = 404;
    resp->body = json_error_body("not found");
    return;
  }

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
    return;
  }

  if (req.method == "GET") {
    if (wants_decisions) {
      resp->status = 404;
      resp->body = json_error_body("not found");
      return;
    }

    AgentDb::ApprovalRequestRow row;
    std::string db_err;
    if (!db_or_null->get_approval_request(approval_id, &row, &db_err)) {
      resp->status = 404;
      resp->body = json_error_body(db_err.empty() ? "approval not found" : db_err);
      return;
    }

    std::vector<AgentDb::ApprovalDecisionRow> decisions;
    if (!db_or_null->list_approval_decisions(approval_id, &decisions, &db_err)) {
      resp->status = 500;
      resp->body = json_error_body(db_err.empty() ? "failed to list decisions" : db_err);
      return;
    }

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["approval"] = approval_row_to_json(row);
    Json::Value arr(Json::arrayValue);
    for (const auto& d : decisions) arr.append(decision_row_to_json(d));
    out["decisions"] = arr;
    resp->body = json_stringify(out);
    return;
  }

  if (req.method == "POST") {
    if (!wants_decisions) {
      resp->status = 404;
      resp->body = json_error_body("not found");
      return;
    }

    Json::Value body;
    std::string perr;
    if (!json_parse_object(req.body, &body, &perr)) {
      resp->status = 400;
      resp->body = json_error_body(perr.empty() ? "invalid json" : perr);
      return;
    }

    const std::string member_id = body.isMember("member_id") && body["member_id"].isString() ? trim_copy(body["member_id"].asString()) : "";
    const std::string decision = body.isMember("decision") && body["decision"].isString() ? lower_copy(trim_copy(body["decision"].asString())) : "";
    const std::string note = body.isMember("note") && body["note"].isString() ? body["note"].asString() : "";

    if (member_id.empty()) {
      resp->status = 400;
      resp->body = json_error_body("missing member_id");
      return;
    }
    if (!(decision == "approve" || decision == "deny")) {
      resp->status = 400;
      resp->body = json_error_body("invalid decision (approve|deny)");
      return;
    }

    AgentDb::ApprovalRequestRow row;
    std::string db_err;
    if (!db_or_null->get_approval_request(approval_id, &row, &db_err)) {
      resp->status = 404;
      resp->body = json_error_body(db_err.empty() ? "approval not found" : db_err);
      return;
    }
    if (!row.status.empty() && lower_copy(row.status) != "pending") {
      resp->status = 409;
      resp->body = json_error_body("approval already resolved");
      return;
    }

    AgentDb::ApprovalDecisionRow dr;
    dr.approval_id = approval_id;
    dr.member_id = member_id;
    dr.decision = decision;
    dr.note = note;
    if (!db_or_null->insert_approval_decision(dr, &db_err)) {
      resp->status = 500;
      resp->body = json_error_body(db_err.empty() ? "failed to insert decision" : db_err);
      return;
    }

    std::vector<AgentDb::ApprovalDecisionRow> decisions;
    if (!db_or_null->list_approval_decisions(approval_id, &decisions, &db_err)) {
      resp->status = 500;
      resp->body = json_error_body(db_err.empty() ? "failed to list decisions" : db_err);
      return;
    }

    const int required = std::max(1, row.required_approvals);
    int approved = 0;
    bool denied = false;
    count_decisions(decisions, &approved, &denied);
    if (denied) {
      row.status = "denied";
      row.decision_reason = "denied";
      (void)db_or_null->update_approval_status(approval_id, row.status, row.decision_reason, nullptr);
    } else if (approved >= required) {
      row.status = "approved";
      row.decision_reason = "approved";
      (void)db_or_null->update_approval_status(approval_id, row.status, row.decision_reason, nullptr);
    }

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["approval_id"] = approval_id;
    out["status"] = row.status.empty() ? "pending" : row.status;
    out["approved"] = approved;
    out["required_approvals"] = required;
    if (!decisions.empty()) {
      out["decision"] = decision_row_to_json(decisions.back());
    }
    resp->body = json_stringify(out);
    return;
  }

  resp->status = 405;
  resp->body = json_error_body("method not allowed");
}

}  // namespace agentd
