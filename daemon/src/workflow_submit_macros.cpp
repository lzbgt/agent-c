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

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path[0] == '/') return base + path;
  return base + "/" + path;
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
    if (kind != "delegate_parallel" && kind != "edge_parallel" && kind != "agentd_parallel") {
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

    if (kind == "agentd_parallel") {
      const Json::Value ap = t.isMember("agentd_parallel") ? t["agentd_parallel"] : Json::Value(Json::nullValue);
      if (!ap.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel task missing agentd_parallel object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      const Json::Value targets = ap.isMember("targets") ? ap["targets"] : Json::Value(Json::nullValue);
      if (!targets.isArray() || targets.empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_parallel.targets must be a non-empty array";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      int64_t count = (int64_t)targets.size();
      if (count > 32) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.targets too large (max 32)";
          o["task_id"] = task_id;
          o["count"] = (Json::Int64)count;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      const Json::Value call =
        ap.isMember("agentd_call") ? ap["agentd_call"] : Json::Value(Json::nullValue);
      if (!call.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_parallel.agentd_call must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (call.isMember("base_url") && call["base_url"].isString() && !trim_copy(call["base_url"].asString()).empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_call.base_url must be omitted (targets provide base_url)";
          o["task_id"] = task_id;
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
      std::unordered_set<std::string> seen_target_ids;
      for (Json::ArrayIndex ti = 0; ti < targets.size(); ti++) {
        const Json::Value& tgt = targets[ti];

        std::string base_url;
        std::string target_id;
        bool allow_error = true;
        Json::Value target_expect(Json::nullValue);
        Json::Value target_max_attempts(Json::nullValue);

        if (tgt.isString()) {
          base_url = trim_copy(tgt.asString());
          target_id = "t" + std::to_string((int)ti);
        } else if (tgt.isObject()) {
          if (tgt.isMember("base_url") && tgt["base_url"].isString()) base_url = trim_copy(tgt["base_url"].asString());
          if (tgt.isMember("id") && tgt["id"].isString()) target_id = trim_copy(tgt["id"].asString());
          // Convenience: broker-proxy addressing mode (compute base_url from {broker_base_url, agent_id}).
          if (base_url.empty() && tgt.isMember("broker_proxy") && !tgt["broker_proxy"].isNull()) {
            const Json::Value bp = tgt["broker_proxy"];
            if (!bp.isObject()) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy must be an object";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            const std::string broker_base_url =
              bp.isMember("broker_base_url") && bp["broker_base_url"].isString() ? trim_copy(bp["broker_base_url"].asString()) : "";
            const std::string agent_id =
              bp.isMember("agent_id") && bp["agent_id"].isString() ? trim_copy(bp["agent_id"].asString()) : "";
            if (broker_base_url.empty() || agent_id.empty()) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy must include broker_base_url and agent_id";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            if (!id_is_safe(agent_id)) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy.agent_id must be id-safe";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                o["agent_id"] = agent_id;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            base_url = join_base_path(broker_base_url, "/v1/agents/" + agent_id + "/proxy");
            if (target_id.empty()) target_id = agent_id;
          }
          if (target_id.empty()) target_id = "t" + std::to_string((int)ti);
          if (tgt.isMember("allow_error") && tgt["allow_error"].isBool()) allow_error = tgt["allow_error"].asBool();
          if (tgt.isMember("expect") && tgt["expect"].isObject()) target_expect = tgt["expect"];
          if (tgt.isMember("max_attempts") && tgt["max_attempts"].isInt()) target_max_attempts = tgt["max_attempts"];
        } else {
          continue;
        }

        if (base_url.empty()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.targets entry missing base_url";
            o["task_id"] = task_id;
            o["index"] = (Json::Int64)ti;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (target_id.empty() || !id_is_safe(target_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.targets[].id must be id-safe";
            o["task_id"] = task_id;
            o["index"] = (Json::Int64)ti;
            o["id"] = target_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (!seen_target_ids.insert(target_id).second) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "duplicate agentd_parallel target id";
            o["task_id"] = task_id;
            o["id"] = target_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        const std::string attempt_task_id = task_id + ":" + target_id;
        if (!id_is_safe(attempt_task_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel produced invalid derived task_id";
            o["task_id"] = task_id;
            o["target_id"] = target_id;
            o["derived_task_id"] = attempt_task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        Json::Value at(Json::objectValue);
        at["task_id"] = attempt_task_id;
        at["kind"] = "agentd_call";
        at["allow_error"] = allow_error;
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

        if (target_max_attempts.isInt()) {
          at["max_attempts"] = target_max_attempts;
        } else if (t.isMember("max_attempts") && t["max_attempts"].isInt()) {
          at["max_attempts"] = t["max_attempts"];
        }
        if (target_expect.isObject()) at["expect"] = target_expect;

        Json::Value call2 = call;
        call2["base_url"] = base_url;
        at["agentd_call"] = call2;

        out.append(at);
        attempt_task_ids.append(attempt_task_id);
      }

      if (attempt_task_ids.empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.targets must include at least one valid entry";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
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
      if (ap.isMember("aggregate") && !ap["aggregate"].isNull()) {
        if (!ap["aggregate"].isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.agentd_parallel.aggregate must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        agg = ap["aggregate"];
      }

      // agentd_parallel default join behavior: first_ok across targets.
      if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
        agg["mode"] = "first_ok";
      }
      agg["task_ids"] = attempt_task_ids;

      const std::string agg_mode =
        agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
      if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
          o["task_id"] = task_id;
          o["mode"] = agg_mode;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Ergonomic default for agentd_parallel quorum: node identity is the remote agent base_url.
      // Aggregate's default node_pointer (/edge/node_id) is correct for edge_invoke, but agentd_call results
      // identify the target under /agentd/base_url.
      if (agg_mode == "quorum_hashes") {
        const bool has_ptrs =
          agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
        if (!has_ptrs) {
          Json::Value ptrs(Json::arrayValue);
          ptrs.append("/agentd/result_sha256");
          agg["pointers"] = ptrs;
        }

        const bool has_node_pointer =
          agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
        if (!has_node_pointer) agg["node_pointer"] = "/agentd/base_url";
      }

      join["aggregate"] = agg;

      if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
      if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

      out.append(join);
      continue;
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

      // Ergonomic defaults for edge_parallel quorum:
      // - pointers defaults to the stable edge result hash surface
      // - node_pointer defaults to the selected node identity (for distinct-node quorum)
      if (agg_mode == "quorum_hashes") {
        const bool has_ptrs =
          agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
        if (!has_ptrs) {
          Json::Value ptrs(Json::arrayValue);
          ptrs.append("/edge_result_sha256");
          agg["pointers"] = ptrs;
        }
        const bool has_node_pointer =
          agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
        if (!has_node_pointer) agg["node_pointer"] = "/edge/node_id";
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
        clamp_key("max_tool_call_args_chars");
        clamp_key("max_tool_result_chars");
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

    // Ergonomic defaults for delegate_parallel quorum:
    // - pointers defaults to assistant_text (common surface for attempt outputs)
    // - note: aggregate's global default pointers (/avm/result_hash, /avm/trace_hash) are not meaningful for LLM attempts
    if (agg_mode == "quorum_hashes") {
      const bool has_ptrs =
        agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
      if (!has_ptrs) {
        Json::Value ptrs(Json::arrayValue);
        ptrs.append("/assistant_text");
        agg["pointers"] = ptrs;
      }
      // Distinct-node quorum: use provider base_url as the default node identity for run-attempt tasks.
      // This enables require_distinct_nodes for multi-provider correctness checks.
      const bool has_node_pointer =
        agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
      if (!has_node_pointer) agg["node_pointer"] = "/effective_base_url";
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
