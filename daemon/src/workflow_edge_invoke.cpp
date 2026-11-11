#include "workflow_edge_invoke.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_engine_common.h"
#include <algorithm>
#include <unordered_set>

namespace agentd {
using namespace workflow_engine_internal;
Json::Value workflow_edge_invoke_to_json(AgentDb *db,
                                         const AgentDb::WorkflowRow &wf,
                                         const AgentDb::WorkflowTaskRow &task,
                                         const Json::Value &rr) {
  Json::Value e = rr.isMember("edge") && rr["edge"].isObject()
                      ? rr["edge"]
                      : Json::Value(Json::nullValue);
  Json::Value out(Json::objectValue);
  out["kind"] = "edge_invoke";
  out["ok"] = false;

  // The durable assignment, not a later scheduler result, owns node choice.
  AgentDb::EdgeTaskRow assigned;
  if (db &&
      db->get_edge_task(wf.workflow_id, task.task_id, &assigned, nullptr)) {
    if (!e.isObject())
      e = Json::Value(Json::objectValue);
    const auto requested = e.get("node_id", "").asString();
    if (!requested.empty() && requested != assigned.node_id) {
      out["error"] = "device assignment conflicts with its durable node";
      return out;
    }
    e["node_id"] = assigned.node_id;
  }

  if (!db || !db->is_open()) {
    out["error"] = "db not available";
    out["retryable"] = true;
    out["retry_in_ms"] = 250;
  } else if (!e.isObject()) {
    out["error"] = "edge config must be an object";
  } else {
    std::string node_id = e.isMember("node_id") && e["node_id"].isString()
                              ? e["node_id"].asString()
                              : "";

    if (node_id.empty() && e.isMember("match_any") &&
        e["match_any"].isObject()) {
      const auto &m = e["match_any"];
      auto read_arr = [&](const char *k, std::vector<std::string> *outv) {
        if (!outv)
          return;
        outv->clear();
        if (!m.isMember(k) || !m[k].isArray())
          return;
        for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
          if (!m[k][i].isString())
            continue;
          const std::string s = m[k][i].asString();
          if (!s.empty())
            outv->push_back(s);
        }
      };
      std::vector<std::string> requires_tools;
      std::vector<std::string> tags_all;
      std::vector<std::string> tags_any;
      std::vector<std::string> tags_none;
      std::unordered_set<std::string> exclude_node_ids;
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
      (void)edge_select_node_match_any(db, requires_tools, tags_all, tags_any,
                                       tags_none, ex, &node_id);
      if (!node_id.empty())
        e["node_id"] = node_id;
    }

    const std::string mode = e.isMember("mode") && e["mode"].isString()
                                 ? e["mode"].asString()
                                 : "invoke";
    const std::string tool_name =
        e.isMember("tool") && e["tool"].isString() ? e["tool"].asString() : "";
    const Json::Value args = e.isMember("args") && e["args"].isObject()
                                 ? e["args"]
                                 : Json::Value(Json::nullValue);

    int64_t deadline_utc_ms = 0;
    if (e.isMember("deadline_utc_ms") &&
        (e["deadline_utc_ms"].isInt64() || e["deadline_utc_ms"].isUInt64())) {
      deadline_utc_ms = e["deadline_utc_ms"].isInt64()
                            ? e["deadline_utc_ms"].asInt64()
                            : (int64_t)e["deadline_utc_ms"].asUInt64();
    } else if (e.isMember("timeout_ms") &&
               (e["timeout_ms"].isInt64() || e["timeout_ms"].isUInt64() ||
                e["timeout_ms"].isInt())) {
      const int64_t tmo = e["timeout_ms"].asInt64();
      const int64_t tmo2 =
          std::max<int64_t>(100, std::min<int64_t>(600000, tmo));
      deadline_utc_ms = edge_unix_ms_now() + tmo2;
    } else {
      deadline_utc_ms = edge_unix_ms_now() + 5000;
    }

    std::unordered_set<std::string> allow_hazards;
    if (e.isMember("allow_hazards") && e["allow_hazards"].isArray()) {
      for (Json::ArrayIndex i = 0; i < e["allow_hazards"].size(); i++) {
        if (e["allow_hazards"][i].isString())
          allow_hazards.insert(e["allow_hazards"][i].asString());
      }
    }
    const bool allow_high_side_effect =
        e.isMember("allow_high_side_effect") &&
                e["allow_high_side_effect"].isBool()
            ? e["allow_high_side_effect"].asBool()
            : false;

    std::string idempotency_key =
        e.isMember("idempotency_key") && e["idempotency_key"].isString()
            ? e["idempotency_key"].asString()
            : "";
    if (idempotency_key.empty())
      idempotency_key = wf.workflow_id + ":" + task.task_id;

    if (node_id.empty()) {
      out["error"] = "missing node_id (or match_any did not select)";
    } else {
      Json::Value payload(Json::objectValue);
      if (mode == "invoke") {
        if (tool_name.empty() || !args.isObject()) {
          out["error"] = "missing tool/args";
          out["edge"] = e;
          // fall through to persist out
          payload = Json::Value(Json::nullValue);
        } else {
          payload["tool"] = tool_name;
          payload["args"] = args;
        }
      } else if (mode == "agent") {
        if (e.isMember("payload") && e["payload"].isObject()) {
          payload = e["payload"];
        } else if (e.isMember("prompt") && e["prompt"].isString() &&
                   !e["prompt"].asString().empty()) {
          payload["prompt"] = e["prompt"].asString();
        } else {
          out["error"] = "missing edge.payload (object) or edge.prompt "
                         "(string) for mode=agent";
          out["edge"] = e;
          payload = Json::Value(Json::nullValue);
        }
      } else {
        out["error"] = "unsupported edge mode (expected invoke|agent)";
        out["edge"] = e;
        payload = Json::Value(Json::nullValue);
      }

      if (!payload.isObject()) {
        // Invalid payload/mode; no enqueue.
      } else {
        // Enqueue TASK_ASSIGN (idempotent via UNIQUE(node_id,idempotency_key)
        // and PK(task_id,step_id)).

        int64_t outbox_id = 0;
        bool deduped = false;
        std::string derr;
        int http = 500;
        Json::Value trace(Json::nullValue);
        if (!wf.trace_id.empty()) {
          trace = Json::Value(Json::objectValue);
          trace["trace_id"] = wf.trace_id;
        }
        const bool enq_ok = edge_enqueue_task_assign(
            db, node_id, wf.workflow_id, task.task_id, idempotency_key, mode,
            deadline_utc_ms, task.attempt, payload, trace, allow_hazards,
            allow_high_side_effect,
            /*enforce_safety=*/true,
            /*enforce_rate_limit=*/true, &outbox_id, &deduped, &derr, &http);

        Json::Value edge(Json::objectValue);
        edge["node_id"] = node_id;
        edge["task_id"] = wf.workflow_id;
        edge["step_id"] = task.task_id;
        edge["idempotency_key"] = idempotency_key;
        edge["deadline_utc_ms"] = (Json::Int64)(assigned.deadline_utc_ms > 0
                                                    ? assigned.deadline_utc_ms
                                                    : deadline_utc_ms);
        edge["outbox_id"] = (Json::Int64)outbox_id;
        edge["deduped"] = deduped;
        out["tool_calls_total"] = Json::Int64(enq_ok && !deduped ? 1 : 0);
        out["steps_executed"] = Json::Int64(enq_ok && !deduped ? 1 : 0);
        edge["mode"] = mode;
        out["edge"] = edge;

        if (!enq_ok) {
          out["error"] = derr.empty() ? "failed to enqueue edge task" : derr;
        } else {
          AgentDb::EdgeTaskRow tr;
          std::string terr;
          if (!db->get_edge_task(wf.workflow_id, task.task_id, &tr, &terr)) {
            out["error"] = "edge task not found after enqueue";
            out["retryable"] = true;
            out["retry_in_ms"] = 100;
          } else {
            out["edge_platform_state"] = tr.state;
            AgentDb::EdgeTaskRow receipt;
            if (tr.state != "FAILED" &&
                db->get_edge_task_receipt(tr, &receipt, &terr)) {
              tr = receipt;
              out["edge_receipt_reconciled"] = true;
            } else if (!terr.empty()) {
              out["error"] = "cannot inspect device receipt: " + terr;
              out["retryable"] = true;
              return out;
            }
            out["edge_state"] = tr.state;
            if (!tr.result_sha256.empty())
              out["edge_result_sha256"] = tr.result_sha256;
            if (!tr.result_json.empty()) {
              Json::Value v;
              std::string perr3;
              if (json_parse_any(tr.result_json, &v, &perr3))
                out["edge_result"] = v;
            }
            if (!tr.attest_json.empty()) {
              Json::Value v;
              std::string perr3;
              if (json_parse_any(tr.attest_json, &v, &perr3) && v.isObject())
                out["edge_attest"] = v;
            }
            if (!tr.error.empty())
              out["edge_error"] = tr.error;

            if (tr.state == "SUCCEEDED") {
              out["ok"] = true;
              if (out.isMember("edge_result"))
                out["assistant_text"] =
                    json_stringify_compact_local(out["edge_result"]);
              else
                out["assistant_text"] = "edge:SUCCEEDED";
            } else if (edge_is_terminal_task_state(tr.state)) {
              out["ok"] = false;
              out["error"] =
                  tr.error.empty() ? "edge task " + tr.state : tr.error;
              if (tr.state == "TIMED_OUT" || tr.state == "CANCELED") {
                out["needs_input"] = true;
                out["retryable"] = false;
                out["blocker"]["id"] =
                    "device:" + wf.workflow_id + ":" + task.task_id;
                out["blocker"]["reason"] = out["error"];
                out["blocker"]["attempts"].append(
                    "Loaded the original durable device assignment and its "
                    "current outcome.");
                out["blocker"]["attempts"].append(
                    "No matching successful device receipt is recorded; the "
                    "original command was not repeated.");
                out["blocker"]["kind"] = "device_outcome_unknown";
                out["blocker"]["workflow_id"] = wf.workflow_id;
                out["blocker"]["task_id"] = task.task_id;
                out["blocker"]["node_id"] = tr.node_id;
                out["blocker"]["required_action"] =
                    "Obtain the original device result, then check its receipt. "
                    "An elapsed deadline or cancellation is not proof of "
                    "physical rollback.";
              }
            } else {
              out["ok"] = false;
              out["error"] = "edge task pending";
              out["retryable"] = true;
              out["retry_in_ms"] = 100;
              out["assistant_text"] = "edge:" + tr.state;
            }
          }
        }
      }
    }
  }
  return out;
}
} // namespace agentd
