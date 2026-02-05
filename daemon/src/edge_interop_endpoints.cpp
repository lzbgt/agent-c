#include "edge_interop_endpoints.h"

#include "daemon_auth.h"
#include "edge_rules.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoints.h"

#include "agent_sha256.h"

#include <json/json.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static bool select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  std::string* out_node_id
) {
  return edge_select_node_match_any(db, requires_tools, tags_all, tags_any, tags_none, out_node_id);
}

}  // namespace

void handle_edge_message_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value env;
  std::string perr;
  if (!json_parse_object(req.body, &env, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  const std::string msg_id = env.isMember("msg_id") && env["msg_id"].isString() ? trim_copy(env["msg_id"].asString()) : "";
  const int64_t ts_utc_ms = env.isMember("ts_utc_ms") && env["ts_utc_ms"].isInt64() ? env["ts_utc_ms"].asInt64()
    : (env.isMember("ts_utc_ms") && env["ts_utc_ms"].isUInt64() ? (int64_t)env["ts_utc_ms"].asUInt64() : 0);
  const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
  const std::string from_id = env.isMember("from") && env["from"].isString() ? trim_copy(env["from"].asString()) : "";
  std::string to_id;
  if (env.isMember("to")) {
    if (env["to"].isString()) to_id = trim_copy(env["to"].asString());
    else if (env["to"].isNull()) to_id.clear();
    else {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid envelope.to (expected string|null)\"}";
      return;
    }
  }
  const Json::Value body = env.isMember("body") ? env["body"] : Json::Value(Json::nullValue);
  const Json::Value trace = env.isMember("trace") && env["trace"].isObject() ? env["trace"] : Json::Value(Json::nullValue);

  if (msg_id.empty() || type.empty() || !body.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope (missing msg_id/type/body)\"}";
    return;
  }
  if (!edge_id_is_safe(type) || msg_id.size() > 128) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope msg_id/type\"}";
    return;
  }

  // Persist inbound (dedupe by msg_id).
  bool deduped = false;
  {
    AgentDb::EdgeInboxMessageRow ir;
    ir.msg_id = msg_id;
    ir.ts_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : edge_unix_ms_now();
    ir.type = type;
    ir.from_id = from_id;
    ir.to_id = to_id;
    ir.envelope_json = edge_json_stringify_compact(env);
    std::string err;
    if (!db_or_null->insert_edge_inbox_message(ir, &deduped, &err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist edge inbox message";
      o["detail"] = err;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    if (deduped) o["deduped"] = true;
    resp->body = edge_json_stringify_compact(o);
  }

  const int64_t now = edge_unix_ms_now();
  if (deduped) {
    bool processed = false;
    std::string perr2;
    const bool found = db_or_null->get_edge_inbox_message_processed(msg_id, &processed, &perr2);
    if (found && processed) {
      // Already processed: keep the deduped response and return without re-applying side effects.
      Json::Value o(Json::objectValue);
      o["ok"] = true;
      o["deduped"] = true;
      o["processed"] = true;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
  }

  struct InboxProcessedGuard {
    AgentDb* db = nullptr;
    std::string msg_id;
    int64_t now = 0;
    HttpResponse* resp = nullptr;
    bool armed = false;
    ~InboxProcessedGuard() {
      if (!armed || !db || msg_id.empty() || !resp) return;
      // Mark as processed for any non-5xx response. 5xx errors are considered retryable.
      if (resp->status >= 500) return;
      std::string ign;
      (void)db->mark_edge_inbox_message_processed(msg_id, now, &ign);
    }
  };

  InboxProcessedGuard inbox_guard;
  inbox_guard.db = db_or_null;
  inbox_guard.msg_id = msg_id;
  inbox_guard.now = now;
  inbox_guard.resp = resp;
  inbox_guard.armed = true;

  auto sanitize_id_token = [](std::string s, size_t max_len) -> std::string {
    if (s.size() > max_len) s.resize(max_len);
    if (s.empty()) return s;
    for (char& c : s) {
      const bool ok =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == ':';
      if (!ok) c = '_';
    }
    // Avoid leading/trailing underscores from weird msg_ids; best-effort.
    while (!s.empty() && s.front() == '_') s.erase(s.begin());
    while (!s.empty() && s.back() == '_') s.pop_back();
    if (s.empty()) s = "msg";
    if (s.size() > max_len) s.resize(max_len);
    return s;
  };

  auto try_send_outbox_ack = [&](const std::string& ack_type, const Json::Value& ack_body) {
    std::string reply_node_id;
    if (from_id.rfind("node:", 0) == 0) reply_node_id = from_id.substr(5);
    if (body.isMember("node_id") && body["node_id"].isString()) reply_node_id = trim_copy(body["node_id"].asString());
    if (reply_node_id.empty() || !edge_id_is_safe(reply_node_id)) return;
    Json::Value ack(Json::objectValue);
    ack["msg_id"] = edge_make_uuidish_msg_id();
    ack["ts_utc_ms"] = (Json::Int64)now;
    ack["type"] = ack_type;
    ack["from"] = "platform";
    ack["to"] = edge_node_to_prefix(reply_node_id);
    ack["body"] = ack_body.isObject() ? ack_body : Json::Value(Json::objectValue);
    AgentDb::EdgeOutboxMessageRow orow;
    orow.node_id = reply_node_id;
    orow.ts_utc_ms = now;
    orow.envelope_json = edge_json_stringify_compact(ack);
    (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
  };

  // Durable workflow handoff over UM‑BMP ingress:
  // - allows resource-constrained nodes (MCU agent_core) to hand off durable orchestration to the platform
  // - transport-agnostic; works via MQTT/LoRa gateways mapped to /api/v1/edge/message
  if (type == "DURABLE_WORKFLOW_SUBMIT") {
    Json::Value wfargs = body;
    if (body.isMember("workflow") && body["workflow"].isObject()) wfargs = body["workflow"];
    if (!wfargs.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid DURABLE_WORKFLOW_SUBMIT body (expected object)\"}";
      return;
    }

    // Canonicalize workflow_id/idempotency_key using msg_id (retry-safe at transport level).
    const std::string token = sanitize_id_token(msg_id, 96);
    std::string wid =
      wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
    if (wid.empty()) wid = std::string("wf:") + token;
    wfargs["workflow_id"] = sanitize_id_token(wid, 128);

    if (!wfargs.isMember("trace_id") || !wfargs["trace_id"].isString() || wfargs["trace_id"].asString().empty()) {
      wfargs["trace_id"] = wfargs["workflow_id"];
    }

    if (!wfargs.isMember("idempotency_key") || !wfargs["idempotency_key"].isString() || wfargs["idempotency_key"].asString().empty()) {
      // Prefer workflow-scoped idempotency when workflow_id is caller-provided, so callers can safely retry even if
      // a transport bridge regenerates msg_id. If workflow_id is derived from msg_id, this still de-dupes within that
      // message id.
      const std::string wf_token =
        wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
      const std::string ik = !wf_token.empty()
        ? (std::string("edge_wf:") + wf_token)
        : (std::string("edge_msg:") + token);
      wfargs["idempotency_key"] = sanitize_id_token(ik, 128);
    }

    // Safety: nodes should not send inline API keys by default.
    wfargs["allow_inline_api_keys"] = false;

    HttpRequest req2 = req;
    req2.body = json_stringify(wfargs);
    HttpResponse r2;
    handle_workflow_submit_endpoint(cfg, cors_cfg, db_or_null, req2, &r2);
    *resp = r2;

    // Best-effort: notify the node via outbox.
    Json::Value ack_body(Json::objectValue);
    ack_body["ok"] = (resp->status >= 200 && resp->status < 300);
    ack_body["workflow_id"] = wfargs["workflow_id"];
    ack_body["op"] = "submit";
    try_send_outbox_ack("DURABLE_WORKFLOW_ACK", ack_body);
    return;
  }

  if (type == "DURABLE_WORKFLOW_CANCEL") {
    const std::string workflow_id = body.isMember("workflow_id") && body["workflow_id"].isString() ? trim_copy(body["workflow_id"].asString()) : "";
    if (workflow_id.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
      return;
    }
    Json::Value args(Json::objectValue);
    args["workflow_id"] = workflow_id;
    HttpRequest req2 = req;
    req2.body = json_stringify(args);
    HttpResponse r2;
    handle_workflow_cancel_endpoint(cfg, cors_cfg, db_or_null, req2, &r2);
    *resp = r2;

    Json::Value ack_body(Json::objectValue);
    ack_body["ok"] = (resp->status >= 200 && resp->status < 300);
    ack_body["workflow_id"] = workflow_id;
    ack_body["op"] = "cancel";
    try_send_outbox_ack("DURABLE_WORKFLOW_ACK", ack_body);
    return;
  }

  if (type == "NODE_HELLO" || type == "NODE_HEARTBEAT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string model = body.isMember("model") && body["model"].isString() ? body["model"].asString() : "";
    const std::string fw = body.isMember("fw_git_sha") && body["fw_git_sha"].isString() ? body["fw_git_sha"].asString() : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    if (node_id.empty() || !edge_id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid node_id\"}";
      return;
    }
    if (!caps_sha.empty() && !edge_sha256_token_is_safe(caps_sha)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid caps_sha256\"}";
      return;
    }

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.model = model;
    nr.fw_git_sha = fw;
    nr.caps_sha256 = caps_sha;
    if (type == "NODE_HELLO") nr.last_hello_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node record";
      o["detail"] = uerr;
      resp->body = edge_json_stringify_compact(o);
      return;
    }

    // Best-effort: if caps sha is unknown or changed, request full manifest.
    bool need_caps = false;
    if (!caps_sha.empty()) {
      AgentDb::EdgeNodeRow existing;
      std::string err;
      if (db_or_null->get_edge_node(node_id, &existing, &err)) {
        if (existing.caps_sha256 != caps_sha || existing.manifest_json.empty()) {
          need_caps = true;
        }
      } else {
        need_caps = true;
      }
    }
    if (need_caps) {
      Json::Value env(Json::objectValue);
      env["msg_id"] = edge_make_uuidish_msg_id();
      env["ts_utc_ms"] = (Json::Int64)now;
      env["type"] = "PLATFORM_CAPS_REQ";
      env["from"] = "platform";
      env["to"] = edge_node_to_prefix(node_id);
      Json::Value b(Json::objectValue);
      b["node_id"] = node_id;
      b["want"] = "full";
      env["body"] = b;

      AgentDb::EdgeOutboxMessageRow orow;
      orow.node_id = node_id;
      orow.ts_utc_ms = now;
      orow.envelope_json = edge_json_stringify_compact(env);
      int64_t outbox_id = 0;
      std::string err;
      (void)db_or_null->insert_edge_outbox_message(orow, &outbox_id, &err);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_CAPS_RSP") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const Json::Value manifest = body.isMember("manifest") ? body["manifest"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !edge_id_is_safe(node_id) || !manifest.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid NODE_CAPS_RSP body\"}";
      return;
    }

    std::string tags_json, tools_json, hw_json;
    edge_manifest_extract_best_effort(manifest, &tags_json, &tools_json, &hw_json);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    if (manifest.isMember("caps_sha256") && manifest["caps_sha256"].isString()) {
      const std::string caps_sha = trim_copy(manifest["caps_sha256"].asString());
      if (!caps_sha.empty() && !edge_sha256_token_is_safe(caps_sha)) {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"invalid manifest.caps_sha256\"}";
        return;
      }
      nr.caps_sha256 = caps_sha;
    }
    nr.manifest_json = edge_json_stringify_compact(manifest);
    nr.tags_json = tags_json;
    nr.tools_json = tools_json;
    nr.hardware_presence_json = hw_json;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node manifest";
      o["detail"] = uerr;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  auto update_task_state = [&](const std::string& task_id, const std::string& step_id, const std::string& state,
                               const std::string& result_json, const std::string& error_text, const Json::Value& event_data) {
    if (task_id.empty() || step_id.empty() || state.empty()) return;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (!db_or_null->get_edge_task(task_id, step_id, &tr, &terr)) {
      // Unknown task; ignore to keep ingestion robust.
      return;
    }
    const bool terminal = edge_is_terminal_task_state(tr.state);
    const int64_t ts_eff = ts_utc_ms > 0 ? ts_utc_ms : now;

    std::string got_idem;
    if (event_data.isObject() && event_data.isMember("idempotency_key") && event_data["idempotency_key"].isString()) {
      got_idem = trim_copy(event_data["idempotency_key"].asString());
    }
    const bool idempotency_mismatch =
      (!got_idem.empty() && !tr.idempotency_key.empty() && got_idem != tr.idempotency_key);

    std::string msg_trace_id;
    bool msg_trace_id_valid = true;
    if (event_data.isObject() && event_data.isMember("trace") && event_data["trace"].isObject() && event_data["trace"].isMember("trace_id") &&
        event_data["trace"]["trace_id"].isString()) {
      msg_trace_id = trim_copy(event_data["trace"]["trace_id"].asString());
      if (!msg_trace_id.empty() && !edge_trace_id_is_safe(msg_trace_id)) msg_trace_id_valid = false;
    }
    const std::string msg_trace_id_eff = msg_trace_id_valid ? msg_trace_id : "";
    const bool can_backfill_trace_id = !idempotency_mismatch && !msg_trace_id_eff.empty() && tr.trace_id.empty();

    // Do not regress/override terminal states set by the platform (e.g. deadline sweeper) or earlier completion.
    // We still persist the event for observability.
    const bool apply_update = !terminal && !idempotency_mismatch;
    const std::string effective_state = apply_update ? state : tr.state;
    bool attest_result_sha_mismatch = false;
    std::string attest_result_sha;
    if (apply_update) {
      if (state == "SUCCEEDED" && !result_json.empty()) {
        char hex[65] = {0};
        agent_sha256_hex_of_bytes(result_json.data(), result_json.size(), hex);
        tr.result_sha256 = std::string("sha256:") + hex;
      }
      if (state == "SUCCEEDED" && event_data.isObject() && event_data.isMember("result") && event_data["result"].isObject()) {
        const Json::Value result = event_data["result"];
        if (result.isMember("attest") && result["attest"].isObject()) {
          std::string aj = edge_json_stringify_compact(result["attest"]);
          if (aj.size() > 8192) aj.resize(8192);
          tr.attest_json = aj;

          const Json::Value at = result["attest"];
          if (at.isMember("result_sha256") && at["result_sha256"].isString()) {
            attest_result_sha = trim_copy(at["result_sha256"].asString());
            if (!attest_result_sha.empty() && edge_sha256_token_is_safe(attest_result_sha) && !tr.result_sha256.empty() &&
                attest_result_sha != tr.result_sha256) {
              attest_result_sha_mismatch = true;
            }
          }
        }
      }
    }
    if (apply_update) {
      tr.state = state;
      tr.updated_utc_ms = ts_eff;
      if (!result_json.empty()) tr.result_json = result_json;
      if (!error_text.empty()) tr.error = error_text;
      if (can_backfill_trace_id) tr.trace_id = msg_trace_id_eff;
      (void)db_or_null->upsert_edge_task(tr, nullptr);
    } else if (can_backfill_trace_id) {
      tr.trace_id = msg_trace_id_eff;
      tr.updated_utc_ms = std::max<int64_t>(tr.updated_utc_ms, ts_eff);
      (void)db_or_null->upsert_edge_task(tr, nullptr);
    }
    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = ts_eff;
    ev.state = state;
    Json::Value d = event_data.isNull() ? Json::Value(Json::objectValue) : event_data;
    // If the incoming message omitted trace, but the platform has a stored trace_id for this task,
    // inject it so trace correlation remains durable across lossy/legacy transports.
    if (!msg_trace_id_valid && !msg_trace_id.empty()) {
      d["_trace_id_invalid"] = true;
      d["_msg_trace_id"] = msg_trace_id;
      if (d.isMember("trace")) d.removeMember("trace");
    }
    if (msg_trace_id_eff.empty() && !tr.trace_id.empty()) {
      Json::Value trc(Json::objectValue);
      trc["trace_id"] = tr.trace_id;
      d["trace"] = trc;
    } else if (!tr.trace_id.empty() && !msg_trace_id_eff.empty() && msg_trace_id_eff != tr.trace_id) {
      d["_trace_id_mismatch"] = true;
      d["_platform_trace_id"] = tr.trace_id;
      d["_msg_trace_id"] = msg_trace_id_eff;
    }
    if (idempotency_mismatch) {
      d["_ignored_by_platform"] = true;
      d["_reason"] = "idempotency_key_mismatch";
      d["_platform_idempotency_key"] = tr.idempotency_key;
      d["_msg_idempotency_key"] = got_idem;
    } else if (terminal && state != tr.state) {
      d["_ignored_by_platform"] = true;
      d["_platform_state"] = tr.state;
    }
    if (attest_result_sha_mismatch) {
      d["_attest_result_sha256_mismatch"] = true;
      d["_attest_result_sha256"] = attest_result_sha;
      d["_platform_result_sha256"] = tr.result_sha256;
    }
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_task_event(ev, nullptr, nullptr);

    // Best-effort: if this task belongs to an edge workflow (task_id == workflow_id), reflect state into the step.
    if (apply_update || state == effective_state) {
      AgentDb::EdgeWorkflowRow wf;
      std::string werr;
      if (db_or_null->get_edge_workflow(task_id, &wf, &werr)) {
        std::vector<AgentDb::EdgeWorkflowStepRow> steps;
        std::string serr;
        if (db_or_null->list_edge_workflow_steps(task_id, &steps, &serr)) {
          for (auto& s : steps) {
            if (s.step_id != step_id) continue;
            if (s.state != effective_state) {
              s.state = effective_state;
              s.updated_utc_ms = ts_eff;
              if (!error_text.empty()) s.error = error_text;
              (void)db_or_null->upsert_edge_workflow_step(s, nullptr);
            }
            AgentDb::EdgeWorkflowEventRow wev;
            wev.workflow_id = task_id;
            wev.ts_utc_ms = ts_eff;
            wev.type = "step_state";
            Json::Value wd(Json::objectValue);
            wd["workflow_id"] = task_id;
            wd["step_id"] = step_id;
            wd["state"] = state;
            if (!got_idem.empty()) wd["idempotency_key"] = got_idem;
            if (!error_text.empty()) wd["error"] = error_text;
            if (idempotency_mismatch) wd["_ignored_by_platform"] = true;
            wev.data_json = edge_json_stringify_compact(wd);
            (void)db_or_null->insert_edge_workflow_event(wev, nullptr, nullptr);
            break;
          }
        }
      }
    }
  };

  if (type == "TASK_ACK") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_ACK body\"}";
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_ACK body (missing/invalid idempotency_key)\"}";
      return;
    }
    if (!body.isMember("accepted") || !body["accepted"].isBool()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_ACK body (missing accepted: bool)\"}";
      return;
    }
    const bool accepted = body["accepted"].asBool();
    std::string reason;
    if (body.isMember("reason") && body["reason"].isString()) reason = body["reason"].asString();
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    if (accepted) {
      update_task_state(task_id, step_id, "QUEUED", /*result_json=*/"", /*error_text=*/"", d);
    } else {
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", reason.empty() ? "rejected" : reason, d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_EVENT") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string state = body.isMember("state") && body["state"].isString() ? body["state"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || state.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_EVENT body\"}";
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_EVENT body (missing/invalid idempotency_key)\"}";
      return;
    }
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    std::string result_json;
    if (body.isMember("result")) result_json = edge_json_stringify_compact(body["result"]);
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    update_task_state(task_id, step_id, state, result_json, error, d);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_DONE") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_DONE body\"}";
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_DONE body (missing/invalid idempotency_key)\"}";
      return;
    }
    if (!body.isMember("result")) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_DONE body (missing result)\"}";
      return;
    }

    // Optional correctness guardrail: if the node manifest includes a `result_schema` for this tool,
    // validate `result.data` before marking the task SUCCEEDED (prevents malformed outputs from flowing into workflows).
    //
    // Policy: treat schema mismatch as a terminal FAILED (fail-closed). This is limited to mode=invoke tasks.
    std::string result_schema_error;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (db_or_null && db_or_null->get_edge_task(task_id, step_id, &tr, &terr) && tr.mode == "invoke" && !tr.node_id.empty() &&
        !tr.tool_name.empty()) {
      AgentDb::EdgeNodeRow nr;
      std::string nerr;
      if (db_or_null->get_edge_node(tr.node_id, &nr, &nerr) && !nr.manifest_json.empty()) {
        Json::Value manifest;
        std::string merr;
        if (json_parse_any(nr.manifest_json, &manifest, &merr) && manifest.isObject()) {
          Json::Value schema;
          std::string serr;
          if (edge_tool_result_schema_from_manifest_best_effort(manifest, tr.tool_name, &schema, &serr) && schema.isObject()) {
            const Json::Value result = body["result"];
            if (!result.isObject()) {
              result_schema_error = "result must be an object";
            } else if (!result.isMember("data")) {
              result_schema_error = "missing result.data";
            } else {
              const Json::Value data = result["data"];
              std::string verr;
              if (!edge_json_schema_subset_validate_best_effort(schema, data, "result.data", &verr)) {
                result_schema_error = verr.empty() ? "result_schema mismatch" : verr;
              }
            }
          }
        }
      }
    }

    std::string result_json;
    if (body.isMember("result")) result_json = edge_json_stringify_compact(body["result"]);
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    if (!result_schema_error.empty()) {
      d["_result_schema_error"] = result_schema_error;
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", std::string("result_schema_mismatch: ") + result_schema_error, d);
    } else {
      update_task_state(task_id, step_id, "SUCCEEDED", result_json, /*error_text=*/"", d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_FAILED") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string idempotency_key =
      body.isMember("idempotency_key") && body["idempotency_key"].isString() ? trim_copy(body["idempotency_key"].asString()) : "";
    if (task_id.empty() || step_id.empty() || !edge_id_is_safe(task_id) || !edge_id_is_safe(step_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_FAILED body\"}";
      return;
    }
    if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_FAILED body (missing/invalid idempotency_key)\"}";
      return;
    }
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    if (error.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid TASK_FAILED body (missing error: string)\"}";
      return;
    }
    Json::Value d = body;
    if (trace.isObject()) d["trace"] = trace;
    update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", error, d);
    resp->body = "{\"ok\":true}";
    return;
  }

  // Node-initiated collaboration: allow nodes to submit/cancel edge workflows via the same UM‑BMP message ingress.
  //
  // This is a platform-side extension beyond the strict UM‑EAIS v0.1 draft; it enables:
  // - sensor nodes to ask the platform to orchestrate multi-node workflows without pre-configured rules
  // - embedded agents to “handoff” an intent to the platform coordinator
  if (type == "WORKFLOW_SUBMIT") {
    Json::Value wfargs = body;
    if (body.isMember("workflow") && body["workflow"].isObject()) wfargs = body["workflow"];
    if (!wfargs.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid WORKFLOW_SUBMIT body (expected object)\"}";
      return;
    }

    std::string workflow_id = wfargs.isMember("workflow_id") && wfargs["workflow_id"].isString() ? trim_copy(wfargs["workflow_id"].asString()) : "";
    if (workflow_id.empty()) workflow_id = std::string("wf:") + edge_make_uuidish_msg_id();
    if (!edge_id_is_safe(workflow_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid workflow_id\"}";
      return;
    }

    const std::string goal = wfargs.isMember("goal") && wfargs["goal"].isString() ? wfargs["goal"].asString() : "";
    int priority = 0;
    if (wfargs.isMember("priority") && (wfargs["priority"].isInt() || wfargs["priority"].isUInt())) {
      priority = wfargs["priority"].isInt() ? wfargs["priority"].asInt() : (int)std::min((Json::UInt)INT32_MAX, wfargs["priority"].asUInt());
    }

    if (!wfargs.isMember("steps") || !wfargs["steps"].isArray() || wfargs["steps"].empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"missing/invalid steps (expected non-empty array)\"}";
      return;
    }

    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    steps.reserve(wfargs["steps"].size());

    for (Json::ArrayIndex i = 0; i < wfargs["steps"].size(); i++) {
      const auto& s = wfargs["steps"][i];
      if (!s.isObject()) continue;
      const std::string step_id = s.isMember("step_id") && s["step_id"].isString() ? trim_copy(s["step_id"].asString()) : "";
      const std::string kind = s.isMember("kind") && s["kind"].isString() ? trim_copy(s["kind"].asString()) : "";
      if (step_id.empty() || !edge_id_is_safe(step_id) || kind.empty()) {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"invalid step (missing step_id/kind)\"}";
        return;
      }
      if (kind != "invoke_tool" && kind != "run_agent" && kind != "join") {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"unsupported step.kind\"}";
        return;
      }

      Json::Value depends(Json::arrayValue);
      if (s.isMember("depends_on") && s["depends_on"].isArray()) depends = s["depends_on"];
      Json::Value target = s.isMember("target") ? s["target"] : Json::Value(Json::objectValue);
      Json::Value payload = s.isMember("payload") ? s["payload"] : Json::Value(Json::objectValue);
      if (kind != "join" && !target.isObject()) {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"invalid step.target (expected object)\"}";
        return;
      }
      if (!payload.isObject()) {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"invalid step.payload (expected object)\"}";
        return;
      }

      std::string join_mode = s.isMember("join_mode") && s["join_mode"].isString() ? trim_copy(s["join_mode"].asString()) : "";
      if (!join_mode.empty() && join_mode != "all" && join_mode != "any") {
        resp->status = 400;
        resp->body = "{\"ok\":false,\"error\":\"invalid join_mode (expected all|any)\"}";
        return;
      }

      int64_t deadline_utc_ms = 0;
      if (s.isMember("deadline_utc_ms") && (s["deadline_utc_ms"].isInt64() || s["deadline_utc_ms"].isUInt64())) {
        deadline_utc_ms = s["deadline_utc_ms"].isInt64() ? s["deadline_utc_ms"].asInt64() : (int64_t)s["deadline_utc_ms"].asUInt64();
      }
      if (kind != "join" && deadline_utc_ms <= 0) deadline_utc_ms = now + 60000;

      int max_attempts = 1;
      if (s.isMember("max_attempts") && (s["max_attempts"].isInt() || s["max_attempts"].isUInt())) {
        max_attempts = s["max_attempts"].isInt() ? s["max_attempts"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["max_attempts"].asUInt());
      }
      if (max_attempts < 1) max_attempts = 1;
      if (max_attempts > 100) max_attempts = 100;

      int backoff_ms = 0;
      if (s.isMember("backoff_ms") && (s["backoff_ms"].isInt() || s["backoff_ms"].isUInt())) {
        backoff_ms = s["backoff_ms"].isInt() ? s["backoff_ms"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["backoff_ms"].asUInt());
      }
      if (backoff_ms < 0) backoff_ms = 0;
      if (backoff_ms > 600000) backoff_ms = 600000;

      AgentDb::EdgeWorkflowStepRow row;
      row.workflow_id = workflow_id;
      row.step_id = step_id;
      row.kind = kind;
      row.depends_on_json = edge_json_stringify_compact(depends);
      row.target_json = edge_json_stringify_compact(target);
      row.payload_json = edge_json_stringify_compact(payload);
      row.join_mode = join_mode;
      row.deadline_utc_ms = deadline_utc_ms;
      row.attempt = 0;
      row.max_attempts = (kind == "join") ? 1 : max_attempts;
      row.next_ready_utc_ms = 0;
      row.backoff_ms = (kind == "join") ? 0 : backoff_ms;
      row.state = "PENDING";
      row.created_utc_ms = now;
      row.updated_utc_ms = now;
      steps.push_back(std::move(row));
    }

    AgentDb::EdgeWorkflowRow wf;
    wf.workflow_id = workflow_id;
    wf.goal = goal;
    wf.status = "QUEUED";
    wf.priority = priority;
    wf.spec_json = edge_json_stringify_compact(wfargs);
    wf.created_utc_ms = now;
    wf.updated_utc_ms = now;

    std::string werr;
    if (!db_or_null->create_edge_workflow(wf, steps, &werr)) {
      AgentDb::EdgeWorkflowRow existing;
      std::string gerr;
      if (!db_or_null->get_edge_workflow(workflow_id, &existing, &gerr)) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to create edge workflow";
        o["detail"] = werr;
        resp->body = edge_json_stringify_compact(o);
        return;
      }
    }

    {
      AgentDb::EdgeWorkflowEventRow ev;
      ev.workflow_id = workflow_id;
      ev.ts_utc_ms = now;
      ev.type = "workflow_created";
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      if (!goal.empty()) d["goal"] = goal;
      d["priority"] = priority;
      if (!from_id.empty()) d["submitted_from"] = from_id;
      d["steps"] = (Json::Int64)steps.size();
      ev.data_json = edge_json_stringify_compact(d);
      (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);
    }

    // Best-effort: send an explicit ACK to the submitting node via outbox, so non-HTTP transports can observe it.
    std::string reply_node_id;
    if (from_id.rfind("node:", 0) == 0) reply_node_id = from_id.substr(5);
    if (body.isMember("node_id") && body["node_id"].isString()) reply_node_id = trim_copy(body["node_id"].asString());
    if (!reply_node_id.empty() && edge_id_is_safe(reply_node_id)) {
      Json::Value ack(Json::objectValue);
      ack["msg_id"] = edge_make_uuidish_msg_id();
      ack["ts_utc_ms"] = (Json::Int64)now;
      ack["type"] = "WORKFLOW_ACK";
      ack["from"] = "platform";
      ack["to"] = edge_node_to_prefix(reply_node_id);
      Json::Value b(Json::objectValue);
      b["workflow_id"] = workflow_id;
      b["ok"] = true;
      ack["body"] = b;
      AgentDb::EdgeOutboxMessageRow orow;
      orow.node_id = reply_node_id;
      orow.ts_utc_ms = now;
      orow.envelope_json = edge_json_stringify_compact(ack);
      (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
    }

    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["workflow_id"] = workflow_id;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  if (type == "WORKFLOW_CANCEL") {
    const std::string workflow_id = body.isMember("workflow_id") && body["workflow_id"].isString()
      ? trim_copy(body["workflow_id"].asString())
      : "";
    if (workflow_id.empty() || !edge_id_is_safe(workflow_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"missing/invalid workflow_id\"}";
      return;
    }
    AgentDb::EdgeWorkflowRow wf;
    std::string werr;
    if (!db_or_null->get_edge_workflow(workflow_id, &wf, &werr)) {
      resp->status = 404;
      resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
      return;
    }
    if (wf.status != "CANCELED" && wf.status != "SUCCEEDED" && wf.status != "FAILED") {
      wf.status = "CANCELED";
      wf.updated_utc_ms = now;
      (void)db_or_null->upsert_edge_workflow(wf, nullptr);
    }

    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    std::string serr;
    (void)db_or_null->list_edge_workflow_steps(workflow_id, &steps, &serr);
    for (auto& s : steps) {
      if (s.state == "SUCCEEDED" || s.state == "FAILED" || s.state == "TIMED_OUT" || s.state == "CANCELED") continue;
      s.state = "CANCELED";
      s.updated_utc_ms = now;
      (void)db_or_null->upsert_edge_workflow_step(s, nullptr);
    }

    AgentDb::EdgeWorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_utc_ms = now;
    ev.type = "workflow_canceled";
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    if (!from_id.empty()) d["canceled_from"] = from_id;
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);

    // Best-effort: send an explicit ACK to the node via outbox so non-HTTP transports can observe cancellation.
    // (Matches the WORKFLOW_SUBMIT ACK pattern.)
    {
      Json::Value ack_body(Json::objectValue);
      ack_body["workflow_id"] = workflow_id;
      ack_body["ok"] = true;
      ack_body["status"] = "CANCELED";
      try_send_outbox_ack("WORKFLOW_ACK", ack_body);
    }

    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["workflow_id"] = workflow_id;
    o["status"] = wf.status;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  if (type == "SENSOR_EVENT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string event_type = body.isMember("event_type") && body["event_type"].isString() ? body["event_type"].asString() : "";
    const int64_t ts2 = body.isMember("ts_utc_ms") && (body["ts_utc_ms"].isInt64() || body["ts_utc_ms"].isUInt64())
      ? (body["ts_utc_ms"].isInt64() ? body["ts_utc_ms"].asInt64() : (int64_t)body["ts_utc_ms"].asUInt64())
      : (ts_utc_ms > 0 ? ts_utc_ms : now);
    const double confidence = body.isMember("confidence") && (body["confidence"].isDouble() || body["confidence"].isInt())
      ? body["confidence"].asDouble()
      : 0.0;
    Json::Value data = body.isMember("data") ? body["data"] : Json::Value(Json::objectValue);
    if (node_id.empty() || !edge_id_is_safe(node_id) || event_type.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid SENSOR_EVENT body\"}";
      return;
    }
    AgentDb::EdgeSensorEventRow sr;
    sr.node_id = node_id;
    sr.event_type = event_type;
    sr.ts_utc_ms = ts2;
    sr.confidence = confidence;
    sr.data_json = edge_json_stringify_compact(data.isNull() ? Json::Value(Json::objectValue) : data);
    (void)db_or_null->insert_edge_sensor_event(sr, nullptr, nullptr);

    edge_rules_apply_for_sensor_event_best_effort(
      db_or_null,
      node_id,
      msg_id,
      event_type,
      ts2,
      confidence,
      data
    );
    resp->body = "{\"ok\":true}";
    return;
  }

  // Unknown message types are accepted and persisted (forward compatible).
  resp->body = "{\"ok\":true}";
}

void handle_edge_outbox_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  int64_t cursor = 0;
  const auto c = query_get(req.query, "cursor");
  if (c && !c->empty()) {
    try { cursor = (int64_t)std::stoll(*c); } catch (...) { cursor = 0; }
  }
  if (cursor < 0) cursor = 0;

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 2048));

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  std::string err;
  if (!db_or_null->list_edge_outbox_messages(*nid, cursor, limit, &msgs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list outbox";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& m : msgs) {
    Json::Value row(Json::objectValue);
    row["outbox_id"] = (Json::Int64)m.outbox_id;
    row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
    Json::Value env;
    std::string perr2;
    if (json_parse_any(m.envelope_json, &env, &perr2) && env.isObject()) {
      row["msg"] = env;
    } else {
      row["msg_raw"] = m.envelope_json;
      row["parse_error"] = perr2;
    }
    arr.append(row);
    cursor_next = std::max(cursor_next, m.outbox_id);
  }
  o["messages"] = arr;
  o["cursor_next"] = (Json::Int64)cursor_next;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_nodes_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db_or_null->list_edge_nodes(limit, &nodes, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list nodes";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  for (const auto& n : nodes) {
    Json::Value row(Json::objectValue);
    row["node_id"] = n.node_id;
    if (!n.model.empty()) row["model"] = n.model;
    if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
    if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
    row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
    row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
    arr.append(row);
  }
  o["nodes"] = arr;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value row(Json::objectValue);
  row["node_id"] = n.node_id;
  if (!n.model.empty()) row["model"] = n.model;
  if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
  if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
  row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
  row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
  if (!n.tags_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(n.tags_json, &v, &perr2) && v.isArray()) row["tags"] = v;
  }
  if (!n.tools_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(n.tools_json, &v, &perr2) && v.isArray()) row["tools"] = v;
  }
  if (!n.hardware_presence_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(n.hardware_presence_json, &v, &perr2) && v.isObject()) row["hardware_presence"] = v;
  }
  if (!n.health_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(n.health_json, &v, &perr2) && v.isObject()) row["health"] = v;
  }
  row["has_manifest"] = !n.manifest_json.empty();
  o["node"] = row;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_caps_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }
  if (n.manifest_json.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node has no manifest\"}";
    return;
  }

  Json::Value m;
  std::string perr;
  if (!json_parse_any(n.manifest_json, &m, &perr) || !m.isObject()) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to parse stored manifest";
    o["parse_error"] = perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["manifest"] = m;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_task_assign_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
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

  std::string node_id = args.isMember("node_id") && args["node_id"].isString() ? trim_copy(args["node_id"].asString()) : "";
  std::vector<std::string> requires_tools;
  std::vector<std::string> tags_all;
  std::vector<std::string> tags_any;
  std::vector<std::string> tags_none;
  if (node_id.empty() && args.isMember("match_any") && args["match_any"].isObject()) {
    const auto& m = args["match_any"];
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

    if (!select_node_match_any(db_or_null, requires_tools, tags_all, tags_any, tags_none, &node_id)) {
      resp->status = 409;
      resp->body = "{\"ok\":false,\"error\":\"no matching node\"}";
      return;
    }
  }
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id (or match_any did not select)\"}";
    return;
  }

  const std::string task_id = args.isMember("task_id") && args["task_id"].isString() ? args["task_id"].asString() : "";
  const std::string step_id = args.isMember("step_id") && args["step_id"].isString() ? args["step_id"].asString() : "";
  const std::string idempotency_key =
    args.isMember("idempotency_key") && args["idempotency_key"].isString() ? args["idempotency_key"].asString() : "";
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? args["mode"].asString() : "";
  const int64_t deadline_utc_ms = args.isMember("deadline_utc_ms") && (args["deadline_utc_ms"].isInt64() || args["deadline_utc_ms"].isUInt64())
    ? (args["deadline_utc_ms"].isInt64() ? args["deadline_utc_ms"].asInt64() : (int64_t)args["deadline_utc_ms"].asUInt64())
    : 0;
  const int attempt = args.isMember("attempt") && (args["attempt"].isInt() || args["attempt"].isUInt())
    ? (args["attempt"].isInt() ? args["attempt"].asInt() : (int)std::min((Json::UInt)INT32_MAX, args["attempt"].asUInt()))
    : 0;
  const Json::Value payload = args.isMember("payload") ? args["payload"] : Json::Value(Json::nullValue);
  const Json::Value trace = args.isMember("trace") ? args["trace"] : Json::Value(Json::nullValue);

  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid task fields\"}";
    return;
  }
  if (!trace.isNull() && !trace.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid trace (expected object)\"}";
    return;
  }

  std::unordered_set<std::string> allow_hazards;
  if (args.isMember("allow_hazards") && args["allow_hazards"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["allow_hazards"].size(); i++) {
      if (args["allow_hazards"][i].isString()) allow_hazards.insert(args["allow_hazards"][i].asString());
    }
  }
  const bool allow_high_side_effect =
    args.isMember("allow_high_side_effect") && args["allow_high_side_effect"].isBool() ? args["allow_high_side_effect"].asBool() : false;

  int64_t outbox_id = 0;
  bool deduped = false;
  std::string derr;
  int http = 500;
  if (!edge_enqueue_task_assign(
        db_or_null,
        node_id,
        task_id,
        step_id,
        idempotency_key,
        mode,
        deadline_utc_ms,
        attempt,
        payload,
        trace,
        allow_hazards,
        allow_high_side_effect,
        /*enforce_safety=*/true,
        /*enforce_rate_limit=*/true,
        &outbox_id,
        &deduped,
        &derr,
        &http)) {
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

void handle_edge_task_get_endpoint(
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
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto tid = query_get(req.query, "task_id");
  const auto sid = query_get(req.query, "step_id");
  if (!tid || tid->empty() || !sid || sid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing task_id/step_id\"}";
    return;
  }

  AgentDb::EdgeTaskRow tr;
  std::string err;
  if (!db_or_null->get_edge_task(*tid, *sid, &tr, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"task not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value t(Json::objectValue);
  t["task_id"] = tr.task_id;
  t["step_id"] = tr.step_id;
  t["node_id"] = tr.node_id;
  t["idempotency_key"] = tr.idempotency_key;
  if (!tr.trace_id.empty()) t["trace_id"] = tr.trace_id;
  if (!tr.result_sha256.empty()) t["result_sha256"] = tr.result_sha256;
  t["mode"] = tr.mode;
  if (!tr.tool_name.empty()) t["tool_name"] = tr.tool_name;
  if (!tr.resource_lock.empty()) t["resource_lock"] = tr.resource_lock;
  t["deadline_utc_ms"] = (Json::Int64)tr.deadline_utc_ms;
  t["state"] = tr.state;
  t["created_utc_ms"] = (Json::Int64)tr.created_utc_ms;
  t["updated_utc_ms"] = (Json::Int64)tr.updated_utc_ms;
  if (!tr.error.empty()) t["error"] = tr.error;
  if (!tr.payload_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.payload_json, &v, &perr2) && v.isObject()) t["payload"] = v;
  }
  if (!tr.result_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.result_json, &v, &perr2)) t["result"] = v;
  }
  if (!tr.attest_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.attest_json, &v, &perr2) && v.isObject()) t["attest"] = v;
  }
  o["task"] = t;
  resp->body = edge_json_stringify_compact(o);
}

}  // namespace agentd
