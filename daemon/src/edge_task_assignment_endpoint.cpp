#include "daemon_auth.h"
#include "edge_interop_endpoints.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include <algorithm>
#include <climits>
#include <unordered_set>
namespace agentd {
void handle_edge_task_assign_endpoint(const DaemonConfig &cfg,
                                      const CorsConfig &cors_cfg,
                                      AgentDb *db_or_null,
                                      const HttpRequest &req,
                                      HttpResponse *resp) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp))
    return;

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

  std::string node_id = args.isMember("node_id") && args["node_id"].isString()
                            ? trim_copy(args["node_id"].asString())
                            : "";
  AgentDb::EdgeTaskRow assignment;
  const auto task_arg = args.get("task_id", Json::Value());
  const auto step_arg = args.get("step_id", Json::Value());
  if (task_arg.isString() && step_arg.isString() &&
      db_or_null->get_edge_task(task_arg.asString(), step_arg.asString(),
                                &assignment, nullptr)) {
    if (!node_id.empty() && node_id != assignment.node_id) {
      resp->status = 409;
      resp->body =
          json_error_body("device assignment conflicts with its durable node");
      return;
    }
    node_id = assignment.node_id;
  }
  std::vector<std::string> requires_tools;
  std::vector<std::string> tags_all;
  std::vector<std::string> tags_any;
  std::vector<std::string> tags_none;
  std::unordered_set<std::string> exclude_node_ids;
  if (node_id.empty() && args.isMember("match_any") &&
      args["match_any"].isObject()) {
    const auto &m = args["match_any"];
    auto read_arr = [&](const char *k, std::vector<std::string> *out) {
      if (!out)
        return;
      out->clear();
      if (!m.isMember(k) || !m[k].isArray())
        return;
      for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
        if (!m[k][i].isString())
          continue;
        out->push_back(m[k][i].asString());
      }
    };
    read_arr("requires_tools", &requires_tools);
    read_arr("tags_all", &tags_all);
    read_arr("tags_any", &tags_any);
    read_arr("tags_none", &tags_none);
    if (m.isMember("exclude_node_ids") && m["exclude_node_ids"].isArray()) {
      for (Json::ArrayIndex i = 0; i < m["exclude_node_ids"].size(); i++) {
        if (!m["exclude_node_ids"][i].isString())
          continue;
        const std::string s = trim_copy(m["exclude_node_ids"][i].asString());
        if (!s.empty())
          exclude_node_ids.insert(s);
      }
    }

    const std::unordered_set<std::string> *ex =
        exclude_node_ids.empty() ? nullptr : &exclude_node_ids;
    if (!edge_select_node_match_any(db_or_null, requires_tools, tags_all,
                                    tags_any, tags_none, ex, &node_id)) {
      resp->status = 409;
      resp->body = json_error_body("no matching node");
      return;
    }
  }
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = json_error_body(
        "missing/invalid node_id (or match_any did not select)");
    return;
  }

  const std::string task_id =
      args.isMember("task_id") && args["task_id"].isString()
          ? args["task_id"].asString()
          : "";
  const std::string step_id =
      args.isMember("step_id") && args["step_id"].isString()
          ? args["step_id"].asString()
          : "";
  const std::string idempotency_key =
      args.isMember("idempotency_key") && args["idempotency_key"].isString()
          ? args["idempotency_key"].asString()
          : "";
  const std::string mode = args.isMember("mode") && args["mode"].isString()
                               ? args["mode"].asString()
                               : "";
  const int64_t deadline_utc_ms =
      args.isMember("deadline_utc_ms") && (args["deadline_utc_ms"].isInt64() ||
                                           args["deadline_utc_ms"].isUInt64())
          ? (args["deadline_utc_ms"].isInt64()
                 ? args["deadline_utc_ms"].asInt64()
                 : (int64_t)args["deadline_utc_ms"].asUInt64())
          : 0;
  const int attempt =
      args.isMember("attempt") &&
              (args["attempt"].isInt() || args["attempt"].isUInt())
          ? (args["attempt"].isInt() ? args["attempt"].asInt()
                                     : (int)std::min((Json::UInt)INT32_MAX,
                                                     args["attempt"].asUInt()))
          : 0;
  const Json::Value payload =
      args.isMember("payload") ? args["payload"] : Json::Value(Json::nullValue);
  const Json::Value trace =
      args.isMember("trace") ? args["trace"] : Json::Value(Json::nullValue);

  if (task_id.empty() || step_id.empty() || idempotency_key.empty() ||
      (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid task fields");
    return;
  }
  if (!trace.isNull() && !trace.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("invalid trace (expected object)");
    return;
  }

  std::unordered_set<std::string> allow_hazards;
  if (args.isMember("allow_hazards") && args["allow_hazards"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["allow_hazards"].size(); i++) {
      if (args["allow_hazards"][i].isString())
        allow_hazards.insert(args["allow_hazards"][i].asString());
    }
  }
  const bool allow_high_side_effect =
      args.isMember("allow_high_side_effect") &&
              args["allow_high_side_effect"].isBool()
          ? args["allow_high_side_effect"].asBool()
          : false;

  int64_t outbox_id = 0;
  bool deduped = false;
  std::string derr;
  int http = 500;
  if (!edge_enqueue_task_assign(
          db_or_null, node_id, task_id, step_id, idempotency_key, mode,
          deadline_utc_ms, attempt, payload, trace, allow_hazards,
          allow_high_side_effect,
          /*enforce_safety=*/true,
          /*enforce_rate_limit=*/true, &outbox_id, &deduped, &derr, &http)) {
    resp->status = http;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = derr.empty() ? "failed to assign edge task" : derr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = node_id;
  o["task_id"] = task_id;
  o["step_id"] = step_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  o["deduped"] = deduped;
  resp->body = edge_json_stringify_compact(o);
}

} // namespace agentd
