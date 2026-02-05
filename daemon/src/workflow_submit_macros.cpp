#include "workflow_submit_macros.h"

#include "edge_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool expand_workflow_submit_macros(
  Json::Value* io_tasks_arr,
  const Json::Value& workflow_defaults,
  AgentDb* db_or_null,
  bool allow_sessions,
  bool allow_inline_api_keys,
  const std::string& session_id,
  const std::string& trace_id,
  HttpResponse* resp
) {
  if (!io_tasks_arr || !io_tasks_arr->isArray()) {
    if (resp) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid tasks (expected array)\"}";
    }
    return false;
  }

  Json::Value out(Json::arrayValue);
  for (Json::ArrayIndex i = 0; i < io_tasks_arr->size(); i++) {
    const Json::Value t = (*io_tasks_arr)[i];
    if (!t.isObject()) {
      out.append(t);
      continue;
    }

    const std::string kind =
      t.isMember("kind") && t["kind"].isString() ? trim_copy(t["kind"].asString()) : std::string();
    if (kind != "delegate_parallel" && kind != "edge_parallel") {
      out.append(t);
      continue;
    }

    const std::string task_id =
      t.isMember("task_id") && t["task_id"].isString() ? trim_copy(t["task_id"].asString()) : ("task_" + std::to_string((int)i));
    if (!id_is_safe(task_id)) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task_id";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    if (kind == "edge_parallel") {
      const Json::Value ep = t.isMember("edge_parallel") ? t["edge_parallel"] : Json::Value(Json::nullValue);
      if (!ep.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel task missing edge_parallel object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!db_or_null || !db_or_null->is_open()) {
        if (resp) {
          resp->status = 503;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "db not available (edge_parallel requires node registry)";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      int64_t count = 0;
      if (ep.isMember("count") && (ep["count"].isInt64() || ep["count"].isUInt64() || ep["count"].isInt() || ep["count"].isUInt())) {
        count = std::max<int64_t>(0, ep["count"].asInt64());
      }
      if (count <= 0) count = 2;
      if (count > 32) count = 32;

      Json::Value edge = ep.isMember("edge") && ep["edge"].isObject() ? ep["edge"] : Json::Value(Json::nullValue);
      if (!edge.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (edge.isMember("node_id") && edge["node_id"].isString() && !trim_copy(edge["node_id"].asString()).empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge.node_id must be omitted (use match_any fan-out)";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (!edge.isMember("match_any") || !edge["match_any"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge.match_any must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      const Json::Value m = edge["match_any"];

      auto read_arr = [&](const char* k, std::vector<std::string>* outv) {
        if (!outv) return;
        outv->clear();
        if (!m.isMember(k) || !m[k].isArray()) return;
        for (Json::ArrayIndex j = 0; j < m[k].size(); j++) {
          if (!m[k][j].isString()) continue;
          const std::string s = trim_copy(m[k][j].asString());
          if (!s.empty()) outv->push_back(s);
        }
      };

      std::vector<std::string> requires_tools;
      std::vector<std::string> tags_all;
      std::vector<std::string> tags_any;
      std::vector<std::string> tags_none;
      read_arr("requires_tools", &requires_tools);
      read_arr("tags_all", &tags_all);
      read_arr("tags_any", &tags_any);
      read_arr("tags_none", &tags_none);

      std::unordered_set<std::string> exclude_node_ids;
      if (m.isMember("exclude_node_ids") && m["exclude_node_ids"].isArray()) {
        for (Json::ArrayIndex j = 0; j < m["exclude_node_ids"].size(); j++) {
          if (!m["exclude_node_ids"][j].isString()) continue;
          const std::string s = trim_copy(m["exclude_node_ids"][j].asString());
          if (!s.empty()) exclude_node_ids.insert(s);
        }
      }

      std::vector<AgentDb::EdgeNodeRow> nodes;
      std::string nerr;
      if (!db_or_null->list_edge_nodes(/*max_rows=*/256, &nodes, &nerr)) {
        if (resp) {
          resp->status = 500;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "failed to list edge nodes";
          o["detail"] = nerr;
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      std::vector<std::string> selected_node_ids;
      selected_node_ids.reserve((size_t)count);
      for (const auto& n : nodes) {
        if ((int64_t)selected_node_ids.size() >= count) break;
        if (n.node_id.empty()) continue;
        if (!edge_id_is_safe(n.node_id)) continue;
        if (exclude_node_ids.count(n.node_id)) continue;

        std::unordered_set<std::string> toolset;
        std::unordered_set<std::string> tagset;
        if (!edge_parse_string_set(n.tools_json, &toolset)) continue;
        if (!edge_parse_string_set(n.tags_json, &tagset)) continue;

        bool ok = true;
        for (const auto& tname : requires_tools) {
          if (tname.empty()) continue;
          if (!toolset.count(tname)) { ok = false; break; }
        }
        if (!ok) continue;
        for (const auto& tag : tags_all) {
          if (tag.empty()) continue;
          if (!tagset.count(tag)) { ok = false; break; }
        }
        if (!ok) continue;
        if (!tags_any.empty()) {
          bool any = false;
          for (const auto& tag : tags_any) {
            if (tag.empty()) continue;
            if (tagset.count(tag)) { any = true; break; }
          }
          if (!any) continue;
        }
        for (const auto& tag : tags_none) {
          if (tag.empty()) continue;
          if (tagset.count(tag)) { ok = false; break; }
        }
        if (!ok) continue;

        selected_node_ids.push_back(n.node_id);
        exclude_node_ids.insert(n.node_id);
      }

      if ((int64_t)selected_node_ids.size() < count) {
        if (resp) {
          resp->status = 409;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel: not enough matching nodes";
          o["task_id"] = task_id;
          o["requested"] = (Json::Int64)count;
          o["selected"] = (Json::Int64)selected_node_ids.size();
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Macro task fields to preserve/propagate.
      std::vector<std::string> dep_ids;
      if (t.isMember("depends_on") && t["depends_on"].isArray()) {
        for (Json::ArrayIndex j = 0; j < t["depends_on"].size(); j++) {
          if (!t["depends_on"][j].isString()) continue;
          const std::string dep = trim_copy(t["depends_on"][j].asString());
          if (!dep.empty()) dep_ids.push_back(dep);
        }
      }
      const int priority =
        t.isMember("priority") && t["priority"].isInt() ? std::max(-1000, std::min(1000, t["priority"].asInt())) : 0;

      Json::Value attempt_task_ids(Json::arrayValue);
      for (const auto& node_id : selected_node_ids) {
        const std::string attempt_task_id = task_id + ":" + node_id;
        if (!id_is_safe(attempt_task_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "edge_parallel produced invalid derived task_id";
            o["task_id"] = task_id;
            o["node_id"] = node_id;
            o["derived_task_id"] = attempt_task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        Json::Value at(Json::objectValue);
        at["task_id"] = attempt_task_id;
        at["kind"] = "edge_invoke";
        at["allow_error"] = true;  // allow errors so the join can deterministically decide.
        if (priority != 0) at["priority"] = priority;
        if (t.isMember("inputs") && t["inputs"].isObject()) at["inputs"] = t["inputs"];
        if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
          at["ready_unix_ms"] = t["ready_unix_ms"];
        }
        if (!dep_ids.empty()) {
          Json::Value deps(Json::arrayValue);
          for (const auto& d : dep_ids) deps.append(d);
          at["depends_on"] = deps;
        }
        if (t.isMember("max_attempts") && t["max_attempts"].isInt()) at["max_attempts"] = t["max_attempts"];
        if (t.isMember("expect") && t["expect"].isObject()) at["expect"] = t["expect"];

        Json::Value e2 = edge;
        e2["node_id"] = node_id;
        e2.removeMember("match_any");  // node is fixed by macro expansion
        at["edge"] = e2;
        out.append(at);
        attempt_task_ids.append(attempt_task_id);
      }

      // Replace macro task with aggregate join.
      Json::Value join(Json::objectValue);
      join["task_id"] = task_id;
      join["kind"] = "aggregate";
      if (priority != 0) join["priority"] = priority;
      if (t.isMember("allow_error") && t["allow_error"].isBool()) join["allow_error"] = t["allow_error"];
      if (t.isMember("inputs") && t["inputs"].isObject()) join["inputs"] = t["inputs"];
      if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
        join["ready_unix_ms"] = t["ready_unix_ms"];
      }
      {
        Json::Value deps(Json::arrayValue);
        for (Json::ArrayIndex k = 0; k < attempt_task_ids.size(); k++) deps.append(attempt_task_ids[k]);
        join["depends_on"] = deps;
      }

      Json::Value agg(Json::objectValue);
      if (ep.isMember("aggregate") && !ep["aggregate"].isNull()) {
        if (!ep["aggregate"].isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "edge_parallel.edge_parallel.aggregate must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        agg = ep["aggregate"];
      }

      // edge_parallel default join behavior: strict_all_ok across attempts.
      if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
        agg["mode"] = "strict_all_ok";
      }
      agg["task_ids"] = attempt_task_ids;

      const std::string agg_mode =
        agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
      if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
          o["task_id"] = task_id;
          o["mode"] = agg_mode;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      join["aggregate"] = agg;

      if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
      if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

      out.append(join);
      continue;
    }

    if (!t.isMember("delegate") || !t["delegate"].isObject()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel task missing delegate object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    const Json::Value del = t["delegate"];
    const Json::Value attempt_defaults =
      del.isMember("attempt_defaults") ? del["attempt_defaults"] : Json::Value(Json::nullValue);
    if (del.isMember("attempt_defaults") && !attempt_defaults.isObject() && !attempt_defaults.isNull()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempt_defaults must be an object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }
    const Json::Value attempt_caps =
      del.isMember("attempt_caps") ? del["attempt_caps"] : Json::Value(Json::nullValue);
    if (del.isMember("attempt_caps") && !attempt_caps.isObject() && !attempt_caps.isNull()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempt_caps must be an object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }
    const Json::Value attempts = del.isMember("attempts") ? del["attempts"] : Json::Value(Json::nullValue);
    if (!attempts.isArray() || attempts.empty()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempts must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    std::vector<std::string> dep_ids;
    if (t.isMember("depends_on") && t["depends_on"].isArray()) {
      for (Json::ArrayIndex di = 0; di < t["depends_on"].size(); di++) {
        if (!t["depends_on"][di].isString()) continue;
        dep_ids.push_back(trim_copy(t["depends_on"][di].asString()));
      }
    }

    int priority = 0;
    if (t.isMember("priority") && t["priority"].isInt()) priority = t["priority"].asInt();

    Json::Value attempt_task_ids(Json::arrayValue);
    std::unordered_set<std::string> seen_attempt_ids;
    for (Json::ArrayIndex ai = 0; ai < attempts.size(); ai++) {
      const auto& a = attempts[ai];
      if (!a.isObject()) continue;
      const std::string attempt_id =
        a.isMember("id") && a["id"].isString() ? trim_copy(a["id"].asString()) : ("att_" + std::to_string((int)ai));
      if (attempt_id.empty() || !id_is_safe(attempt_id)) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel.attempts[].id must be id-safe";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!seen_attempt_ids.insert(attempt_id).second) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "duplicate delegate_parallel attempt id";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!a.isMember("request") || !a["request"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel attempt missing request object";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      const std::string attempt_task_id = task_id + ":" + attempt_id;
      if (!id_is_safe(attempt_task_id)) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel produced invalid derived task_id";
          o["task_id"] = task_id;
          o["attempt_task_id"] = attempt_task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      Json::Value at(Json::objectValue);
      at["task_id"] = attempt_task_id;
      at["allow_error"] =
        a.isMember("allow_error") && a["allow_error"].isBool() ? a["allow_error"].asBool() : true;
      if (t.isMember("inputs") && t["inputs"].isObject()) at["inputs"] = t["inputs"];
      if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
        at["ready_unix_ms"] = t["ready_unix_ms"];
      }
      if (!dep_ids.empty()) {
        Json::Value deps(Json::arrayValue);
        for (const auto& d : dep_ids) deps.append(d);
        at["depends_on"] = deps;
      }
      if (priority != 0) at["priority"] = priority;
      if (a.isMember("max_attempts") && a["max_attempts"].isInt()) at["max_attempts"] = a["max_attempts"];
      if (a.isMember("expect") && a["expect"].isObject()) at["expect"] = a["expect"];

      Json::Value areq = a["request"];
      if (!areq.isObject()) areq = Json::Value(Json::objectValue);

      // Delegate-parallel attempt requests behave like normal workflow tasks:
      // - workflow-level defaults are merged
      // - sessions/no_session are defaulted
      // - attempt_defaults (delegate-level) are merged with higher priority than workflow defaults
      if (attempt_defaults.isObject()) {
        for (const auto& k : attempt_defaults.getMemberNames()) {
          if (!areq.isMember(k)) areq[k] = attempt_defaults[k];
        }
      }
      if (workflow_defaults.isObject()) {
        for (const auto& k : workflow_defaults.getMemberNames()) {
          if (!areq.isMember(k)) areq[k] = workflow_defaults[k];
        }
      }

      // attempt_caps: hard maximums on per-attempt run knobs.
      // Semantics: if cap <= 0, ignore. Else: missing => set to cap; present => min(present, cap).
      if (attempt_caps.isObject()) {
        auto clamp_key = [&](const char* k) {
          if (!attempt_caps.isMember(k)) return;
          const Json::Value& capv = attempt_caps[k];
          if (!(capv.isInt64() || capv.isUInt64() || capv.isInt() || capv.isUInt())) return;
          const int64_t cap = std::max<int64_t>(0, capv.asInt64());
          if (cap <= 0) return;
          if (areq.isMember(k) && (areq[k].isInt64() || areq[k].isUInt64() || areq[k].isInt() || areq[k].isUInt())) {
            const int64_t cur = std::max<int64_t>(0, areq[k].asInt64());
            areq[k] = (Json::Int64)std::min<int64_t>(cur, cap);
          } else if (!areq.isMember(k)) {
            areq[k] = (Json::Int64)cap;
          }
        };
        clamp_key("timeout_ms");
        clamp_key("max_steps");
        clamp_key("max_tool_calls_total");
        clamp_key("max_tool_calls_per_tool");
      }

      if (!allow_sessions) {
        areq["no_session"] = true;
        if (!areq.isMember("tools")) areq["tools"] = "none";
      } else if (!session_id.empty()) {
        if (!areq.isMember("session_id") && (!areq.isMember("no_session") || !areq["no_session"].isBool() || !areq["no_session"].asBool())) {
          areq["session_id"] = session_id;
        }
      }

      if (areq.isMember("api_key") && areq["api_key"].isString() && !areq["api_key"].asString().empty() && !allow_inline_api_keys) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "inline api_key is not allowed for durable workflows (set daemon api_key/provider_keys or pass allow_inline_api_keys=true)";
          o["task_id"] = attempt_task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (!areq.isMember("trace_id") || !areq["trace_id"].isString() || areq["trace_id"].asString().empty()) {
        areq["trace_id"] = trace_id + ":" + task_id + ":" + attempt_id;
      }

      at["request"] = areq;
      out.append(at);
      attempt_task_ids.append(attempt_task_id);
    }

    if (attempt_task_ids.empty()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.attempts must include at least one object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    // Replace the macro task with a deterministic aggregate join at the same task_id.
    Json::Value join(Json::objectValue);
    join["task_id"] = task_id;
    join["kind"] = "aggregate";
    if (priority != 0) join["priority"] = priority;
    if (t.isMember("allow_error") && t["allow_error"].isBool()) join["allow_error"] = t["allow_error"];
    if (t.isMember("inputs") && t["inputs"].isObject()) join["inputs"] = t["inputs"];
    if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
      join["ready_unix_ms"] = t["ready_unix_ms"];
    }

    Json::Value deps(Json::arrayValue);
    for (Json::ArrayIndex k = 0; k < attempt_task_ids.size(); k++) deps.append(attempt_task_ids[k]);
    join["depends_on"] = deps;

    Json::Value agg(Json::objectValue);
    if (del.isMember("aggregate") && !del["aggregate"].isNull()) {
      if (!del["aggregate"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel.delegate.aggregate must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      agg = del["aggregate"];
    }

    // delegate_parallel default join behavior: first_ok across attempts.
    if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
      agg["mode"] = "first_ok";
    }

    // Always override task_ids to match the derived attempt tasks (ignores any caller-provided aggregate.task_ids).
    agg["task_ids"] = attempt_task_ids;

    // Backward-compatible convenience: delegate.ok_pointer/value_pointer become defaults for the aggregate join
    // when the aggregate object does not explicitly set them.
    if (del.isMember("ok_pointer") && del["ok_pointer"].isString() && !del["ok_pointer"].asString().empty()) {
      if (!agg.isMember("ok_pointer") || !agg["ok_pointer"].isString() || trim_copy(agg["ok_pointer"].asString()).empty()) {
        agg["ok_pointer"] = del["ok_pointer"];
      }
    }
    if (del.isMember("value_pointer") && del["value_pointer"].isString() && !del["value_pointer"].asString().empty()) {
      if (!agg.isMember("value_pointer") || !agg["value_pointer"].isString() || trim_copy(agg["value_pointer"].asString()).empty()) {
        agg["value_pointer"] = del["value_pointer"];
      }
    }

    const std::string agg_mode =
      agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
    if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
        o["task_id"] = task_id;
        o["mode"] = agg_mode;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }
    join["aggregate"] = agg;

    if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
    if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

    out.append(join);
  }

  *io_tasks_arr = out;
  return true;
}

}  // namespace agentd

