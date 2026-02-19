#include "edge_rules.h"

#include "daemon_auth.h"
#include "edge_util.h"
#include "edge_interop_endpoints.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoints.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace agentd {

void edge_rules_apply_for_sensor_event_best_effort(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& base_req,
  const std::string& sensor_node_id,
  const std::string& sensor_msg_id,
  const std::string& event_type,
  int64_t event_ts_utc_ms,
  double confidence,
  const Json::Value& data
) {
  (void)event_ts_utc_ms;
  if (!db || !db->is_open()) return;
  if (event_type.empty()) return;

  const int64_t now = edge_unix_ms_now();
  std::vector<AgentDb::EdgeRuleRow> rules;
  std::string err;
  if (!db->list_edge_rules(/*max_rows=*/256, &rules, &err)) return;

  for (auto r : rules) {
    if (!r.enabled) continue;
    if (r.event_type != event_type) continue;
    if (confidence < r.min_confidence) continue;
    if (r.cooldown_ms > 0 && r.last_fired_utc_ms > 0) {
      const int64_t since = now - r.last_fired_utc_ms;
      if (since >= 0 && since < (int64_t)r.cooldown_ms) continue;
    }

    Json::Value action;
    std::string perr;
    if (!json_parse_any(r.action_json, &action, &perr) || !action.isObject()) continue;
    const std::string type = action.isMember("type") && action["type"].isString() ? action["type"].asString() : "";
    if (type != "task_assign" && type != "durable_workflow_submit") continue;

    // For durable_workflow_submit, submit a durable workflow to the platform workflow engine.
    // Use-case: SENSOR_EVENT triggers multi-step orchestration (fan-out + joins + LLM reasoning) with durable continuity.
    if (type == "durable_workflow_submit") {
      Json::Value wfargs = action.isMember("workflow") ? action["workflow"] : Json::Value(Json::nullValue);
      if (!wfargs.isObject()) continue;

      // Deterministic workflow_id/idempotency_key based on rule_id and SENSOR_EVENT msg_id (retry-safe).
      std::string wid =
        wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
      if (wid.empty()) wid = std::string("wf:rule:") + r.rule_id + ":msg:" + sensor_msg_id;
      if (!edge_id_is_safe(wid)) wid = std::string("wf:rule:") + edge_make_uuidish_msg_id();
      wfargs["workflow_id"] = wid.size() > 128 ? wid.substr(0, 128) : wid;

      if (!wfargs.isMember("trace_id") || !wfargs["trace_id"].isString() || wfargs["trace_id"].asString().empty()) {
        wfargs["trace_id"] = wfargs["workflow_id"];
      }
      if (!wfargs.isMember("idempotency_key") || !wfargs["idempotency_key"].isString() || wfargs["idempotency_key"].asString().empty()) {
        const std::string ik = std::string("rule:") + r.rule_id + ":msg:" + sensor_msg_id;
        wfargs["idempotency_key"] = ik.size() > 128 ? ik.substr(0, 128) : ik;
      }
      if (!wfargs.isMember("allow_inline_api_keys") || !wfargs["allow_inline_api_keys"].isBool()) {
        // Safety default: rules should not embed provider keys by default.
        wfargs["allow_inline_api_keys"] = false;
      }

      // Inject SENSOR_EVENT evidence into workflow inputs so tasks can template against it.
      Json::Value inputs =
        wfargs.isMember("inputs") && wfargs["inputs"].isObject() ? wfargs["inputs"] : Json::Value(Json::objectValue);
      if (!inputs.isObject()) inputs = Json::Value(Json::objectValue);
      if (!inputs.isMember("sensor_event")) {
        Json::Value se(Json::objectValue);
        se["node_id"] = sensor_node_id;
        se["msg_id"] = sensor_msg_id;
        se["rule_id"] = r.rule_id;
        se["event_type"] = event_type;
        se["ts_utc_ms"] = (Json::Int64)event_ts_utc_ms;
        se["confidence"] = confidence;
        se["data"] = data.isNull() ? Json::Value(Json::objectValue) : data;
        inputs["sensor_event"] = se;
      }
      wfargs["inputs"] = inputs;

      HttpRequest req2 = base_req;
      req2.body = json_stringify(wfargs);
      HttpResponse r2;
      handle_workflow_submit_endpoint(cfg, cors_cfg, db, req2, &r2);
      if (!(r2.status >= 200 && r2.status < 300)) continue;

      r.last_fired_utc_ms = now;
      r.updated_utc_ms = now;
      (void)db->upsert_edge_rule(r, nullptr);
      continue;
    }

    std::string target_node_id;
    if (action.isMember("target") && action["target"].isObject()) {
      const auto& tgt = action["target"];
      if (tgt.isMember("node_id") && tgt["node_id"].isString()) {
        target_node_id = trim_copy(tgt["node_id"].asString());
      } else if (tgt.isMember("match_any") && tgt["match_any"].isObject()) {
        std::vector<std::string> requires_tools, tags_all, tags_any, tags_none;
        const auto& m = tgt["match_any"];
        auto read_arr = [&](const char* k, std::vector<std::string>* out) {
          if (!out) return;
          out->clear();
          if (!m.isMember(k) || !m[k].isArray()) return;
          for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
            if (!m[k][i].isString()) continue;
            out->push_back(m[k][i].asString());
          }
        };
        read_arr("requires_tools", &requires_tools);
        read_arr("tags_all", &tags_all);
        read_arr("tags_any", &tags_any);
        read_arr("tags_none", &tags_none);
        (void)edge_select_node_match_any(db, requires_tools, tags_all, tags_any, tags_none, nullptr, &target_node_id);
      }
    }
    if (target_node_id.empty()) target_node_id = sensor_node_id;

    const std::string mode = action.isMember("mode") && action["mode"].isString() ? action["mode"].asString() : "invoke";
    Json::Value payload = action.isMember("payload") ? action["payload"] : Json::Value(Json::nullValue);
    if (!payload.isObject()) continue;

    int64_t deadline_in_ms = 60000;
    if (action.isMember("deadline_in_ms") && (action["deadline_in_ms"].isInt64() || action["deadline_in_ms"].isUInt64())) {
      deadline_in_ms = action["deadline_in_ms"].isInt64() ? action["deadline_in_ms"].asInt64() : (int64_t)action["deadline_in_ms"].asUInt64();
    }
    if (deadline_in_ms < 1000) deadline_in_ms = 1000;
    if (deadline_in_ms > 24LL * 60 * 60 * 1000) deadline_in_ms = 24LL * 60 * 60 * 1000;
    const int64_t deadline_utc_ms = now + deadline_in_ms;

    std::string task_id = action.isMember("task_id") && action["task_id"].isString() ? action["task_id"].asString() : "";
    if (task_id.empty()) task_id = std::string("rule_") + edge_make_uuidish_msg_id();
    std::string step_id = action.isMember("step_id") && action["step_id"].isString() ? action["step_id"].asString() : "auto";
    std::string idempotency_key = action.isMember("idempotency_key") && action["idempotency_key"].isString() ? action["idempotency_key"].asString() : "";
    if (idempotency_key.empty()) idempotency_key = std::string("rule:") + r.rule_id + ":msg:" + sensor_msg_id;

    // Automation defaults: do NOT allow hazards or high side-effects by default.
    std::unordered_set<std::string> allow_hazards;
    const bool allow_high_side_effect = false;

    int64_t outbox_id = 0;
    bool deduped = false;
    std::string derr;
    int http = 0;
    if (!edge_enqueue_task_assign(
          db,
          target_node_id,
          task_id,
          step_id,
          idempotency_key,
          mode,
          deadline_utc_ms,
          /*attempt=*/0,
          payload,
          /*trace=*/Json::Value(Json::nullValue),
          allow_hazards,
          allow_high_side_effect,
          /*enforce_safety=*/true,
          /*enforce_rate_limit=*/true,
          &outbox_id,
          &deduped,
          &derr,
          &http)) {
      continue;
    }

    r.last_fired_utc_ms = now;
    r.updated_utc_ms = now;
    (void)db->upsert_edge_rule(r, nullptr);
  }
}

void handle_edge_rule_upsert_endpoint(
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
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  std::string rule_id = args.isMember("rule_id") && args["rule_id"].isString() ? trim_copy(args["rule_id"].asString()) : "";
  if (rule_id.empty()) rule_id = std::string("rule:") + edge_make_uuidish_msg_id();
  if (!edge_id_is_safe(rule_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid rule_id");
    return;
  }

  const std::string event_type =
    args.isMember("event_type") && args["event_type"].isString() ? trim_copy(args["event_type"].asString()) : "";
  if (event_type.empty() || !edge_id_is_safe(event_type)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid event_type");
    return;
  }

  const bool enabled = args.isMember("enabled") && args["enabled"].isBool() ? args["enabled"].asBool() : true;

  double min_conf = 0.0;
  if (args.isMember("min_confidence") && (args["min_confidence"].isDouble() || args["min_confidence"].isInt())) {
    min_conf = args["min_confidence"].asDouble();
    if (min_conf < 0.0) min_conf = 0.0;
    if (min_conf > 1.0) min_conf = 1.0;
  }

  int cooldown_ms = 0;
  if (args.isMember("cooldown_ms") && (args["cooldown_ms"].isInt() || args["cooldown_ms"].isUInt())) {
    cooldown_ms = args["cooldown_ms"].isInt() ? std::max(0, args["cooldown_ms"].asInt()) : (int)args["cooldown_ms"].asUInt();
  }

  Json::Value action = args.isMember("action") ? args["action"] : Json::Value(Json::nullValue);
  if (!action.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid action (expected object)");
    return;
  }
  const std::string atype = action.isMember("type") && action["type"].isString() ? action["type"].asString() : "";
  if (atype != "task_assign" && atype != "durable_workflow_submit") {
    resp->status = 400;
    resp->body = json_error_body("unsupported action.type (expected task_assign or durable_workflow_submit)");
    return;
  }
  if (atype == "durable_workflow_submit") {
    if (!action.isMember("workflow") || !action["workflow"].isObject()) {
      resp->status = 400;
      resp->body = json_error_body("durable_workflow_submit requires action.workflow (object)");
      return;
    }
  }
  const std::string action_json = edge_json_stringify_compact(action);
  if (action_json.size() > 200000) {
    resp->status = 413;
    resp->body = json_error_body("action too large");
    return;
  }

  const int64_t now = edge_unix_ms_now();
  AgentDb::EdgeRuleRow existing;
  std::string gerr;
  const bool have_existing = db_or_null->get_edge_rule(rule_id, &existing, &gerr);

  AgentDb::EdgeRuleRow row;
  row.rule_id = rule_id;
  row.enabled = enabled;
  row.event_type = event_type;
  row.min_confidence = min_conf;
  row.cooldown_ms = cooldown_ms;
  row.action_json = action_json;
  row.updated_utc_ms = now;
  if (have_existing) {
    row.created_utc_ms = existing.created_utc_ms;
    row.last_fired_utc_ms = existing.last_fired_utc_ms;
  } else {
    row.created_utc_ms = now;
    row.last_fired_utc_ms = 0;
  }

  std::string err;
  if (!db_or_null->upsert_edge_rule(row, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to upsert rule";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rule_id"] = rule_id;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_rules_list_endpoint(
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
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  size_t limit = 200;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 512));

  std::vector<AgentDb::EdgeRuleRow> rules;
  std::string err;
  if (!db_or_null->list_edge_rules(limit, &rules, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list rules";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  for (const auto& r : rules) {
    Json::Value row(Json::objectValue);
    row["rule_id"] = r.rule_id;
    row["enabled"] = r.enabled;
    row["event_type"] = r.event_type;
    row["min_confidence"] = r.min_confidence;
    row["cooldown_ms"] = r.cooldown_ms;
    row["last_fired_utc_ms"] = (Json::Int64)r.last_fired_utc_ms;
    row["created_utc_ms"] = (Json::Int64)r.created_utc_ms;
    row["updated_utc_ms"] = (Json::Int64)r.updated_utc_ms;
    if (!r.action_json.empty()) {
      Json::Value a;
      std::string perr2;
      if (json_parse_any(r.action_json, &a, &perr2) && a.isObject()) row["action"] = a;
      else row["action_raw"] = r.action_json;
    }
    arr.append(row);
  }
  o["rules"] = arr;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_rule_delete_endpoint(
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
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto rid = query_get(req.query, "rule_id");
  if (!rid || rid->empty() || !edge_id_is_safe(*rid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid rule_id");
    return;
  }

  std::string err;
  if (!db_or_null->delete_edge_rule(*rid, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to delete rule";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rule_id"] = *rid;
  resp->body = edge_json_stringify_compact(o);
}

}  // namespace agentd
