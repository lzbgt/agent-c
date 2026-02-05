#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

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

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static bool is_safe_relpath_md(const std::string& p) {
  if (p.empty()) return false;
  if (p.size() > 300) return false;
  if (p.find('\\') != std::string::npos) return false;
  if (p[0] == '/') return false;
  if (p.find("..") != std::string::npos) return false;
  if (p.find('\0') != std::string::npos) return false;
  const std::string lp = to_lower_ascii(p);
  if (lp.size() < 3 || lp.rfind(".md") != lp.size() - 3) return false;
  return true;
}

static bool expand_workflow_submit_macros(
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

static std::string new_workflow_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);
  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);
  char buf[96];
  (void)snprintf(buf, sizeof(buf), "wf_%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

static std::string redact_json_best_effort(const std::string& json) {
  if (json.empty()) return json;
  Json::Value v;
  std::string perr;
  if (!json_parse_any(json, &v, &perr)) return json;

  std::function<void(Json::Value*)> walk = [&](Json::Value* cur) {
    if (!cur) return;
    if (cur->isObject()) {
      for (const auto& k : cur->getMemberNames()) {
        if (k == "api_key" || k == "Authorization" || k == "auth_token") {
          (*cur)[k] = "***redacted***";
        } else {
          walk(&((*cur)[k]));
        }
      }
    } else if (cur->isArray()) {
      for (Json::ArrayIndex i = 0; i < cur->size(); i++) {
        walk(&((*cur)[i]));
      }
    }
  };
  walk(&v);
  return json_stringify_compact(v);
}

static bool validate_dag_or_error(
  const std::vector<std::string>& task_ids,
  const std::unordered_map<std::string, std::vector<std::string>>& deps,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  std::unordered_set<std::string> nodes(task_ids.begin(), task_ids.end());

  std::unordered_map<std::string, int> indeg;
  indeg.reserve(nodes.size());
  for (const auto& id : nodes) indeg[id] = 0;
  for (const auto& kv : deps) {
    const std::string& t = kv.first;
    if (!nodes.count(t)) continue;
    for (const auto& d : kv.second) {
      if (!nodes.count(d)) {
        if (out_err) *out_err = "unknown dependency: " + d;
        return false;
      }
      indeg[t] += 1;
    }
  }

  std::vector<std::string> q;
  q.reserve(nodes.size());
  for (const auto& kv : indeg) {
    if (kv.second == 0) q.push_back(kv.first);
  }
  size_t idx = 0;
  size_t visited = 0;
	  while (idx < q.size()) {
	    const std::string cur = q[idx++];
	    visited++;
	    // deps map is reverse (task -> deps). Need reverse edges:
	    // Just scan all nodes once (bounded by task count).
	    for (const auto& kv2 : deps) {
	      const std::string& child = kv2.first;
      if (!nodes.count(child)) continue;
      for (const auto& dep : kv2.second) {
        if (dep == cur) {
          indeg[child] -= 1;
          if (indeg[child] == 0) q.push_back(child);
        }
      }
    }
  }
  if (visited != nodes.size()) {
    if (out_err) *out_err = "dependency cycle detected";
    return false;
  }
  return true;
}

static void collect_referenced_task_ids_from_task_template_token(
  const std::string& token,
  std::unordered_set<std::string>* out
) {
  if (!out) return;
  if (token.rfind("task.", 0) != 0) return;

  const std::string rest = token.substr(std::string("task.").size());
  const std::string suffix_text = ".assistant_text";
  const std::string dot_json = ".json:";

  std::string task_id;
  if (rest.size() > suffix_text.size() && rest.rfind(suffix_text) == (rest.size() - suffix_text.size())) {
    task_id = rest.substr(0, rest.size() - suffix_text.size());
  } else {
    const size_t jpos = rest.find(dot_json);
    if (jpos != std::string::npos) {
      task_id = rest.substr(0, jpos);
    }
  }

  if (!task_id.empty() && id_is_safe(task_id)) out->insert(task_id);
}

static void collect_referenced_task_ids_from_json_value(
  const Json::Value& v,
  std::unordered_set<std::string>* out
) {
  if (!out) return;
  if (v.isString()) {
    const std::string s = v.asString();
    size_t i = 0;
    while (i < s.size()) {
      const size_t p = s.find("${task.", i);
      if (p == std::string::npos) break;
      const size_t end = s.find('}', p + 2);
      if (end == std::string::npos) break;
      const std::string token = s.substr(p + 2, end - (p + 2)); // strip ${ ... }
      collect_referenced_task_ids_from_task_template_token(token, out);
      i = end + 1;
    }
    return;
  }
  if (v.isArray()) {
    for (Json::ArrayIndex i = 0; i < v.size(); i++) {
      collect_referenced_task_ids_from_json_value(v[i], out);
    }
    return;
  }
  if (v.isObject()) {
    if (v.isMember("$ref") && v["$ref"].isString()) {
      const std::string ref = trim_copy(v["$ref"].asString());
      collect_referenced_task_ids_from_task_template_token(ref, out);
    }
    for (const auto& k : v.getMemberNames()) {
      collect_referenced_task_ids_from_json_value(v[k], out);
    }
    return;
  }
}

}  // namespace

void handle_workflow_submit_endpoint(
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
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value tasks = args["tasks"];
  if (!tasks.isArray() || tasks.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing tasks (expected non-empty array)\"}";
    return;
  }

  const bool allow_sessions =
    args.isMember("allow_sessions") && args["allow_sessions"].isBool() ? args["allow_sessions"].asBool() : false;
  const bool allow_inline_api_keys =
    args.isMember("allow_inline_api_keys") && args["allow_inline_api_keys"].isBool() ? args["allow_inline_api_keys"].asBool() : false;
  const bool infer_depends_on =
    args.isMember("infer_depends_on") && args["infer_depends_on"].isBool() ? args["infer_depends_on"].asBool() : false;
  if (args.isMember("infer_depends_on") && !args["infer_depends_on"].isBool()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid infer_depends_on (expected boolean)\"}";
    return;
  }

  std::string workflow_id =
    args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) workflow_id = new_workflow_id();
  if (!id_is_safe(workflow_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid workflow_id\"}";
    return;
  }

  std::string trace_id =
    args.isMember("trace_id") && args["trace_id"].isString() ? trim_copy(args["trace_id"].asString()) : "";
  if (!trace_id.empty() && !id_is_safe(trace_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid trace_id\"}";
    return;
  }
  if (trace_id.empty()) {
    trace_id = workflow_id; // stable default
  }

  std::string session_id =
    args.isMember("session_id") && args["session_id"].isString() ? trim_copy(args["session_id"].asString()) : "";
  if (!session_id.empty() && !session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid session_id\"}";
    return;
  }
  if (args.isMember("session_weight")) {
    if (!(args["session_weight"].isInt64() || args["session_weight"].isUInt64() || args["session_weight"].isInt() || args["session_weight"].isUInt())) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid session_weight (expected int >= 1)\"}";
      return;
    }
    const int64_t sw = args["session_weight"].asInt64();
    if (sw < 1) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid session_weight (expected >= 1)\"}";
      return;
    }
    if (!allow_sessions || session_id.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"session_weight requires allow_sessions=true and non-empty session_id\"}";
      return;
    }
    // Canonicalize and clamp: keep scheduling stable under extreme inputs.
    args["session_weight"] = (Json::Int64)std::min<int64_t>(1024, sw);
  }

  std::string idempotency_key =
    args.isMember("idempotency_key") && args["idempotency_key"].isString() ? trim_copy(args["idempotency_key"].asString()) : "";
  if (args.isMember("idempotency_key") && !args["idempotency_key"].isString() && !args["idempotency_key"].isNull()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid idempotency_key (expected string)\"}";
    return;
  }
  if (!idempotency_key.empty() && !id_is_safe(idempotency_key)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid idempotency_key\"}";
    return;
  }
  if (!idempotency_key.empty()) {
    args["idempotency_key"] = idempotency_key; // canonicalize
  }

  const Json::Value defaults =
    args.isMember("defaults") && args["defaults"].isObject() ? args["defaults"] : Json::Value(Json::nullValue);

  // Expand submit-time macros into scheduler-visible tasks before admission control and DAG validation.
  // This ensures:
  // - admission control applies to the expanded task count
  // - derived tasks participate in fairness caps/budgets and deterministic joins
  if (!expand_workflow_submit_macros(&tasks, defaults, db_or_null, allow_sessions, allow_inline_api_keys, session_id, trace_id, resp)) {
    return;
  }
  args["tasks"] = tasks;

  if (tasks.size() > 128) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"too many tasks (max 128)\"}";
    return;
  }

  // Idempotency: if the key is provided, return the existing workflow instead of creating a duplicate.
  if (!idempotency_key.empty()) {
    const std::string session_scope = allow_sessions ? session_id : "";
    AgentDb::WorkflowRow existing;
    std::string derr;
    const bool found = db_or_null->get_workflow_by_idempotency_key(session_scope, idempotency_key, &existing, &derr);
    if (!derr.empty()) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to query idempotency_key";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
    if (found && !existing.workflow_id.empty()) {
      Json::Value o(Json::objectValue);
      o["ok"] = true;
      o["workflow_id"] = existing.workflow_id;
      o["trace_id"] = existing.trace_id;
      if (allow_sessions && !existing.session_id.empty()) o["session_id"] = existing.session_id;
      o["deduped"] = true;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  // Admission control / backpressure: reject submits that would exceed configured inflight task caps.
  //
  // This prevents unbounded DB growth under fan-out storms and gives callers a deterministic retry surface.
  {
    const int max_total = cfg.workflow_admit_max_inflight_tasks_total;
    if (max_total > 0) {
      int64_t cur = 0;
      std::string derr;
      if (!db_or_null->count_workflow_inflight_tasks_total(&cur, &derr)) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to query inflight workflow tasks";
        o["detail"] = derr;
        resp->body = json_stringify_compact(o);
        return;
      }
      const int64_t requested = (int64_t)tasks.size();
      if (cur + requested > (int64_t)max_total) {
        resp->status = 429;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "workflow admission control: too many inflight tasks (daemon total)";
        o["limit_inflight_tasks_total"] = max_total;
        o["inflight_tasks_total"] = (Json::Int64)cur;
        o["requested_tasks"] = (Json::Int64)requested;
        o["retry_after_ms"] = 500;
        resp->body = json_stringify_compact(o);
        return;
      }
    }

    const int max_per_sess = cfg.workflow_admit_max_inflight_tasks_per_session;
    if (max_per_sess > 0) {
      const std::string session_scope = allow_sessions ? session_id : "";
      if (!session_scope.empty()) {
        int64_t cur = 0;
        std::string derr;
        if (!db_or_null->count_workflow_inflight_tasks_for_session(session_scope, &cur, &derr)) {
          resp->status = 500;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "failed to query inflight workflow tasks";
          o["detail"] = derr;
          resp->body = json_stringify_compact(o);
          return;
        }
        const int64_t requested = (int64_t)tasks.size();
        if (cur + requested > (int64_t)max_per_sess) {
          resp->status = 429;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "workflow admission control: too many inflight tasks (session)";
          o["session_id"] = session_scope;
          o["limit_inflight_tasks_per_session"] = max_per_sess;
          o["inflight_tasks_per_session"] = (Json::Int64)cur;
          o["requested_tasks"] = (Json::Int64)requested;
          o["retry_after_ms"] = 500;
          resp->body = json_stringify_compact(o);
          return;
        }
      }
    }
  }

  const Json::Value workflow_inputs =
    args.isMember("inputs") && args["inputs"].isObject() ? args["inputs"] : Json::Value(Json::nullValue);
  if (args.isMember("inputs") && !args["inputs"].isObject() && !args["inputs"].isNull()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid inputs (expected object)\"}";
    return;
  }
  if (workflow_inputs.isObject()) {
    const auto keys = workflow_inputs.getMemberNames();
    if (keys.size() > 256) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"too many inputs (max 256)\"}";
      return;
    }
    for (const auto& k : keys) {
      if (!id_is_safe(k)) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid input name (expected id-safe)";
        o["input"] = k;
        resp->body = json_stringify_compact(o);
        return;
      }
    }
  }

  int workflow_priority = 0;
  if (args.isMember("priority")) {
    if (!args["priority"].isInt()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid priority (expected int)\"}";
      return;
    }
    workflow_priority = args["priority"].asInt();
    if (workflow_priority < -1000) workflow_priority = -1000;
    if (workflow_priority > 1000) workflow_priority = 1000;
    args["priority"] = workflow_priority; // canonicalize
  }
  if (!args.isMember("priority")) args["priority"] = workflow_priority;

  if (args.isMember("deadline_unix_ms")) {
    if (!(args["deadline_unix_ms"].isInt64() || args["deadline_unix_ms"].isUInt64() || args["deadline_unix_ms"].isInt())) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid deadline_unix_ms (expected int64 unix ms)\"}";
      return;
    }
    const int64_t v = args["deadline_unix_ms"].asInt64();
    if (v <= 0) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid deadline_unix_ms (expected > 0)\"}";
      return;
    }
    args["deadline_unix_ms"] = (Json::Int64)v; // canonicalize
  }

  // Optional workflow-level limits (durable budgets enforced by the workflow engine).
  //
  // Note: these are intended to be forward-compatible. Validate known fields; ignore unknown ones.
  if (args.isMember("workflow_limits")) {
    if (!args["workflow_limits"].isObject() && !args["workflow_limits"].isNull()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits (expected object)\"}";
      return;
    }
    if (args["workflow_limits"].isObject()) {
      Json::Value lim = args["workflow_limits"];
      if (lim.isMember("max_tool_calls_total")) {
        const auto& v = lim["max_tool_calls_total"];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_tool_calls_total (expected int >= 0)\"}";
          return;
        }
        const int64_t n = v.asInt64();
        if (n < 0) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_tool_calls_total (expected >= 0)\"}";
          return;
        }
        lim["max_tool_calls_total"] = (Json::Int64)std::min<int64_t>(1000000000LL, n); // canonicalize
      }
      if (lim.isMember("max_steps_total")) {
        const auto& v = lim["max_steps_total"];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_steps_total (expected int >= 0)\"}";
          return;
        }
        const int64_t n = v.asInt64();
        if (n < 0) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_steps_total (expected >= 0)\"}";
          return;
        }
        lim["max_steps_total"] = (Json::Int64)std::min<int64_t>(1000000000LL, n); // canonicalize
      }
      if (lim.isMember("max_elapsed_ms_total")) {
        const auto& v = lim["max_elapsed_ms_total"];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_elapsed_ms_total (expected int >= 0)\"}";
          return;
        }
        const int64_t n = v.asInt64();
        if (n < 0) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_elapsed_ms_total (expected >= 0)\"}";
          return;
        }
        // Clamp to 1 year to keep values sane (actual enforcement also clamps).
        lim["max_elapsed_ms_total"] = (Json::Int64)std::min<int64_t>(365LL * 24LL * 60LL * 60LL * 1000LL, n); // canonicalize
      }
      if (lim.isMember("max_total_tokens")) {
        const auto& v = lim["max_total_tokens"];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_total_tokens (expected int >= 0)\"}";
          return;
        }
        const int64_t n = v.asInt64();
        if (n < 0) {
          resp->status = 400;
          resp->body = "{\"ok\":false,\"error\":\"invalid workflow_limits.max_total_tokens (expected >= 0)\"}";
          return;
        }
        // Clamp to keep values sane; enforcement also clamps.
        lim["max_total_tokens"] = (Json::Int64)std::min<int64_t>(1000000000000LL, n); // canonicalize
      }
      args["workflow_limits"] = lim;
    }
  }

  const int64_t now = unix_ms_now();
  std::unordered_set<std::string> seen_ids;
  std::vector<std::string> task_ids;
  task_ids.reserve(tasks.size());
  std::unordered_map<std::string, std::vector<std::string>> deps_by_task;
  std::vector<AgentDb::WorkflowTaskRow> rows;
  rows.reserve(tasks.size());

  for (Json::ArrayIndex i = 0; i < tasks.size(); i++) {
    const auto& t = tasks[i];
    if (!t.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"task entry is not an object\"}";
      return;
    }
    const std::string task_id =
      t.isMember("task_id") && t["task_id"].isString() ? trim_copy(t["task_id"].asString()) : ("task_" + std::to_string(i));
    if (!id_is_safe(task_id)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid task_id";
      o["task_id"] = task_id;
      resp->body = json_stringify_compact(o);
      return;
    }
    if (!seen_ids.insert(task_id).second) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "duplicate task_id";
      o["task_id"] = task_id;
      resp->body = json_stringify_compact(o);
      return;
    }

	    const std::string kind =
	      t.isMember("kind") && t["kind"].isString() ? trim_copy(t["kind"].asString()) : std::string();
	    const bool is_avm = (kind == "avm_capsule");
	    const bool is_aggregate = (kind == "aggregate");
	    const bool is_edge = (kind == "edge_invoke");
	    const bool is_edge_wait_sensor = (kind == "edge_wait_sensor");
	    const bool is_delay = (kind == "delay");
	    const bool is_delegate = (kind == "delegate");
	    const bool is_memory_put = (kind == "memory_put");
	    const bool is_memory_search = (kind == "memory_search");
	    const bool is_memory_consolidate = (kind == "memory_consolidate");
	    const bool is_special =
	      is_avm || is_aggregate || is_edge || is_edge_wait_sensor || is_delay || is_delegate || is_memory_put || is_memory_search || is_memory_consolidate;

    Json::Value run_req = t.isMember("request") && t["request"].isObject() ? t["request"] : t;
    if (!is_special && defaults.isObject()) {
      for (const auto& k : defaults.getMemberNames()) {
        if (!run_req.isMember(k)) run_req[k] = defaults[k];
      }
    }

    int task_priority = workflow_priority;
    if (run_req.isMember("priority")) {
      if (!run_req["priority"].isInt()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task priority (expected int)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_priority = run_req["priority"].asInt();
    }
    if (t.isMember("priority")) {
      if (!t["priority"].isInt()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task priority (expected int)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_priority = t["priority"].asInt();
    }
    if (task_priority < -1000) task_priority = -1000;
    if (task_priority > 1000) task_priority = 1000;
    if (!is_special) run_req["priority"] = task_priority;

    if (!is_special) {
      if (!allow_sessions) {
        run_req["no_session"] = true;
        if (!run_req.isMember("tools")) run_req["tools"] = "none";
      } else if (!session_id.empty()) {
        // If sessions are allowed, default tasks to the workflow session unless explicitly overridden.
        if (!run_req.isMember("session_id") && (!run_req.isMember("no_session") || !run_req["no_session"].isBool() || !run_req["no_session"].asBool())) {
          run_req["session_id"] = session_id;
        }
      }
    }

    if (!is_special) {
      if (run_req.isMember("api_key") && run_req["api_key"].isString() && !run_req["api_key"].asString().empty() && !allow_inline_api_keys) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "inline api_key is not allowed for durable workflows (set daemon api_key/provider_keys or pass allow_inline_api_keys=true)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
    }

    Json::Value task_req(Json::objectValue);
    if (is_avm) {
      if (!t.isMember("capsule") || !t["capsule"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "avm_capsule task missing capsule object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& cap = t["capsule"];
      if (!cap.isMember("obc_base64") || !cap["obc_base64"].isString() || cap["obc_base64"].asString().empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "avm_capsule task capsule missing obc_base64";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "avm_capsule";
      task_req["capsule"] = cap;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else if (is_aggregate) {
      if (!t.isMember("aggregate") || !t["aggregate"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "aggregate task missing aggregate object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& agg = t["aggregate"];
      if (!agg.isMember("task_ids") || !agg["task_ids"].isArray() || agg["task_ids"].empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "aggregate.task_ids must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "aggregate";
      task_req["aggregate"] = agg;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_edge) {
      if (!t.isMember("edge") || !t["edge"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task missing edge object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& edge = t["edge"];
      const std::string mode =
        edge.isMember("mode") && edge["mode"].isString() ? trim_copy(edge["mode"].asString()) : "invoke";

      const std::string tool = edge.isMember("tool") && edge["tool"].isString() ? trim_copy(edge["tool"].asString()) : "";
      const bool has_args = edge.isMember("args") && edge["args"].isObject();
      const bool has_payload = edge.isMember("payload") && edge["payload"].isObject();
      const bool has_prompt = edge.isMember("prompt") && edge["prompt"].isString() && !trim_copy(edge["prompt"].asString()).empty();
      const bool has_node_id = edge.isMember("node_id") && edge["node_id"].isString() && !trim_copy(edge["node_id"].asString()).empty();
      const bool has_match_any = edge.isMember("match_any") && edge["match_any"].isObject();
      if (!has_node_id && !has_match_any) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task edge missing node_id or match_any";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      if (mode == "invoke") {
        if (tool.empty() || !has_args) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_invoke task edge missing tool/args for mode=invoke";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return;
        }
      } else if (mode == "agent") {
        if (!has_payload && !has_prompt) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_invoke task edge missing payload (object) or prompt (string) for mode=agent";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return;
        }
      } else {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task edge invalid mode (expected invoke|agent)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "edge_invoke";
      task_req["edge"] = edge;
      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_edge_wait_sensor) {
	      if (!t.isMember("edge_wait_sensor") || !t["edge_wait_sensor"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor task missing edge_wait_sensor object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      const auto& ws = t["edge_wait_sensor"];
	      const std::string event_type =
	        ws.isMember("event_type") && ws["event_type"].isString() ? trim_copy(ws["event_type"].asString()) : "";
	      if (event_type.empty() || !edge_id_is_safe(event_type)) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor.event_type must be a non-empty id-safe string";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ws.isMember("node_id") && ws["node_id"].isString()) {
	        const std::string nid = trim_copy(ws["node_id"].asString());
	        if (!nid.empty() && !edge_id_is_safe(nid)) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "edge_wait_sensor.node_id must be id-safe when provided";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	      } else if (ws.isMember("node_id") && !ws["node_id"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor.node_id must be a string";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ws.isMember("min_confidence") && !ws["min_confidence"].isNumeric() && !ws["min_confidence"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor.min_confidence must be numeric";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ws.isMember("since_utc_ms") &&
	          !(ws["since_utc_ms"].isInt64() || ws["since_utc_ms"].isUInt64() || ws["since_utc_ms"].isInt() || ws["since_utc_ms"].isNull())) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor.since_utc_ms must be an int64 unix ms";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ws.isMember("poll_ms") && !(ws["poll_ms"].isInt() || ws["poll_ms"].isUInt() || ws["poll_ms"].isNull())) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "edge_wait_sensor.poll_ms must be an int";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      task_req["kind"] = "edge_wait_sensor";
	      task_req["edge_wait_sensor"] = ws;
	      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_delay) {
      int64_t delay_ms = 0;
      if (t.isMember("delay_ms") && (t["delay_ms"].isInt64() || t["delay_ms"].isUInt64() || t["delay_ms"].isInt())) {
        delay_ms = t["delay_ms"].asInt64();
      } else if (t.isMember("delay_ms")) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delay_ms must be an integer";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      if (delay_ms < 0) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delay_ms must be >= 0";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      if (delay_ms > 600000) delay_ms = 600000;
      task_req["kind"] = "delay";
      task_req["delay_ms"] = (Json::Int64)delay_ms;
      if (t.isMember("result")) {
        if (!t["result"].isObject() && !t["result"].isNull()) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delay.result must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return;
        }
        if (t["result"].isObject()) task_req["result"] = t["result"];
      }
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_put) {
      if (!t.isMember("memory_put") || !t["memory_put"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "memory_put task missing memory_put object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& mp = t["memory_put"];
      const std::string path =
        mp.isMember("path") && mp["path"].isString() && !trim_copy(mp["path"].asString()).empty()
        ? trim_copy(mp["path"].asString())
        : "STRUCTURED.md";
      if (!is_safe_relpath_md(path)) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "memory_put.path must be a safe relative .md path";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      const Json::Value entries = mp.isMember("entries") ? mp["entries"] : Json::Value(Json::nullValue);
      if (!entries.isArray() || entries.empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "memory_put.entries must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      Json::Value entries2(Json::arrayValue);
      int valid = 0;
      for (Json::ArrayIndex ei = 0; ei < entries.size(); ei++) {
        const auto& e = entries[ei];
        if (!e.isObject()) continue;
        const std::string key =
          e.isMember("key") && e["key"].isString() ? trim_copy(e["key"].asString()) : "";
        const std::string value =
          e.isMember("value") && e["value"].isString() ? e["value"].asString() : "";
        if (key.empty() || value.empty() || key.size() > 200) continue;
        Json::Value o(Json::objectValue);
        o["key"] = key;
        o["value"] = value;
        if (e.isMember("kind") && e["kind"].isString() && !trim_copy(e["kind"].asString()).empty()) {
          o["kind"] = trim_copy(e["kind"].asString());
        } else {
          o["kind"] = "fact";
        }
        if (e.isMember("status") && e["status"].isString() && !trim_copy(e["status"].asString()).empty()) {
          o["status"] = trim_copy(e["status"].asString());
        } else {
          o["status"] = "active";
        }
        if (e.isMember("source") && e["source"].isString() && !trim_copy(e["source"].asString()).empty()) {
          o["source"] = trim_copy(e["source"].asString());
        }
        entries2.append(o);
        valid++;
      }
      if (valid == 0) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "memory_put.entries has no valid entries (each entry requires key + value)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      Json::Value mp2(Json::objectValue);
      mp2["path"] = path;
      mp2["entries"] = entries2;
      const bool checkpoint =
        !mp.isMember("checkpoint") || (mp["checkpoint"].isBool() && mp["checkpoint"].asBool());
      mp2["checkpoint"] = checkpoint;
      if (mp.isMember("keep_checkpoints")) {
        if (!mp["keep_checkpoints"].isInt()) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "memory_put.keep_checkpoints must be an int";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return;
        }
        mp2["keep_checkpoints"] = std::max(1, mp["keep_checkpoints"].asInt());
      }

      task_req["kind"] = "memory_put";
      task_req["memory_put"] = mp2;
      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_search) {
	      if (!t.isMember("memory_search") || !t["memory_search"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search task missing memory_search object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      const auto& ms = t["memory_search"];
	      const std::string query =
	        ms.isMember("query") && ms["query"].isString() ? trim_copy(ms["query"].asString()) : "";
	      if (query.empty() || query.size() > 400) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search.query must be a non-empty string (max 400 chars)";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ms.isMember("max_results") && !ms["max_results"].isInt() && !ms["max_results"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search.max_results must be an int";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ms.isMember("daily_days") && !ms["daily_days"].isInt() && !ms["daily_days"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search.daily_days must be an int";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ms.isMember("case_sensitive") && !ms["case_sensitive"].isBool() && !ms["case_sensitive"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search.case_sensitive must be a bool";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (ms.isMember("use_index") && !ms["use_index"].isBool() && !ms["use_index"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_search.use_index must be a bool";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      task_req["kind"] = "memory_search";
	      task_req["memory_search"] = ms;
	      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_consolidate) {
      const Json::Value mc =
        t.isMember("memory_consolidate") && t["memory_consolidate"].isObject() ? t["memory_consolidate"] : Json::Value(Json::nullValue);
      if (!mc.isNull() && !mc.isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "memory_consolidate must be an object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      Json::Value mc2(Json::objectValue);
      if (mc.isObject()) {
        if (mc.isMember("daily_days")) {
          if (!mc["daily_days"].isInt()) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "memory_consolidate.daily_days must be an int";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
            return;
          }
          mc2["daily_days"] = std::max(0, mc["daily_days"].asInt());
        }
        if (mc.isMember("keep_checkpoints")) {
          if (!mc["keep_checkpoints"].isInt()) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "memory_consolidate.keep_checkpoints must be an int";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
            return;
          }
          mc2["keep_checkpoints"] = std::max(1, mc["keep_checkpoints"].asInt());
        }
        if (mc.isMember("max_entries")) {
          if (!mc["max_entries"].isInt()) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "memory_consolidate.max_entries must be an int";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
            return;
          }
          mc2["max_entries"] = std::max(1, mc["max_entries"].asInt());
        }
        if (mc.isMember("dry_run")) {
          if (!mc["dry_run"].isBool()) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "memory_consolidate.dry_run must be a bool";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
            return;
          }
          mc2["dry_run"] = mc["dry_run"];
        }
      }

      task_req["kind"] = "memory_consolidate";
      task_req["memory_consolidate"] = mc2;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else if (is_delegate) {
      if (!t.isMember("delegate") || !t["delegate"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate task missing delegate object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      const auto& del = t["delegate"];
      const Json::Value attempts = del.isMember("attempts") ? del["attempts"] : Json::Value(Json::nullValue);
      if (!attempts.isArray() || attempts.empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate.attempts must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      Json::Value del2(Json::objectValue);
      const bool stop_on_ok =
        del.isMember("stop_on_ok") && del["stop_on_ok"].isBool() ? del["stop_on_ok"].asBool() : true;
      del2["stop_on_ok"] = stop_on_ok;
      if (del.isMember("attempt_caps")) {
        if (!del["attempt_caps"].isObject() && !del["attempt_caps"].isNull()) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate.attempt_caps must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return;
        }
        if (del["attempt_caps"].isObject()) del2["attempt_caps"] = del["attempt_caps"];
      }

      Json::Value arr(Json::arrayValue);
      std::unordered_set<std::string> seen_attempt_ids;
      for (Json::ArrayIndex ai = 0; ai < attempts.size(); ai++) {
        const auto& a = attempts[ai];
        if (!a.isObject()) continue;
        const std::string attempt_id =
          a.isMember("id") && a["id"].isString() ? trim_copy(a["id"].asString()) : ("att_" + std::to_string((int)ai));
        if (attempt_id.empty() || !id_is_safe(attempt_id)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate.attempts[].id must be id-safe";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
          return;
        }
        if (!seen_attempt_ids.insert(attempt_id).second) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "duplicate delegate attempt id";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
          return;
        }
        if (!a.isMember("request") || !a["request"].isObject()) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate attempt missing request object";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
          return;
        }

        Json::Value areq = a["request"];

        // Merge workflow defaults into each attempt request (delegate is special, so normal task defaulting does not apply).
        if (defaults.isObject()) {
          for (const auto& k : defaults.getMemberNames()) {
            if (!areq.isMember(k)) areq[k] = defaults[k];
          }
        }

        // Session/no_session defaults match normal workflow tasks.
        if (!allow_sessions) {
          areq["no_session"] = true;
          if (!areq.isMember("tools")) areq["tools"] = "none";
        } else if (!session_id.empty()) {
          if (!areq.isMember("session_id") && (!areq.isMember("no_session") || !areq["no_session"].isBool() || !areq["no_session"].asBool())) {
            areq["session_id"] = session_id;
          }
        }

        if (areq.isMember("api_key") && areq["api_key"].isString() && !areq["api_key"].asString().empty() && !allow_inline_api_keys) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "inline api_key is not allowed for durable workflows (set daemon api_key/provider_keys or pass allow_inline_api_keys=true)";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
          return;
        }

        if (!areq.isMember("trace_id") || !areq["trace_id"].isString() || areq["trace_id"].asString().empty()) {
          areq["trace_id"] = trace_id + ":" + task_id + ":" + attempt_id;
        }

        Json::Value a2(Json::objectValue);
        a2["id"] = attempt_id;
        a2["request"] = areq;
        if (a.isMember("expect")) {
          if (!a["expect"].isObject() && !a["expect"].isNull()) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "delegate.attempts[].expect must be an object";
            o["task_id"] = task_id;
            o["attempt_id"] = attempt_id;
            resp->body = json_stringify_compact(o);
            return;
          }
          if (a["expect"].isObject()) a2["expect"] = a["expect"];
        }
        arr.append(a2);
      }

      if (arr.empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate.attempts must include at least one object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      del2["attempts"] = arr;
      task_req["kind"] = "delegate";
      task_req["delegate"] = del2;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else {
      if (!run_req.isMember("prompt") || !run_req["prompt"].isString() || run_req["prompt"].asString().empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "task missing prompt";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      // Default trace_id per task (inherits workflow trace_id).
      if (!run_req.isMember("trace_id") || !run_req["trace_id"].isString() || run_req["trace_id"].asString().empty()) {
        run_req["trace_id"] = trace_id + ":" + task_id;
      }
      task_req = run_req;
    }

    // Attach merged inputs to every task (special or normal).
    // - workflow-level args.inputs: defaults for all tasks
    // - per-task inputs: may be set in task object and/or task.request
    // - per-task overrides workflow-level keys
    if (workflow_inputs.isObject() || t.isMember("inputs") || (run_req.isObject() && run_req.isMember("inputs"))) {
      Json::Value merged(Json::objectValue);
      if (workflow_inputs.isObject()) {
        for (const auto& k : workflow_inputs.getMemberNames()) merged[k] = workflow_inputs[k];
      }

      const Json::Value task_inputs_top =
        t.isMember("inputs") ? t["inputs"] : Json::Value(Json::nullValue);
      const Json::Value task_inputs_req =
        (run_req.isObject() && run_req.isMember("inputs")) ? run_req["inputs"] : Json::Value(Json::nullValue);

      auto merge_inputs = [&](const Json::Value& in, const std::string& where) -> bool {
        if (in.isNull()) return true;
        if (!in.isObject()) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = std::string("invalid inputs (expected object) in ") + where;
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return false;
        }
        const auto keys = in.getMemberNames();
        if (keys.size() > 256) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = std::string("too many inputs (max 256) in ") + where;
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
          return false;
        }
        for (const auto& k : keys) {
          if (!id_is_safe(k)) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "invalid input name (expected id-safe)";
            o["task_id"] = task_id;
            o["input"] = k;
            resp->body = json_stringify_compact(o);
            return false;
          }
          merged[k] = in[k];
        }
        return true;
      };

      if (!merge_inputs(task_inputs_top, "task.inputs")) return;
      if (!merge_inputs(task_inputs_req, "task.request.inputs")) return;

      if (!merged.getMemberNames().empty()) task_req["inputs"] = merged;
    }

    Json::Value deps_arr(Json::arrayValue);
    std::vector<std::string> dep_ids;
    if (t.isMember("depends_on") && t["depends_on"].isArray()) {
      deps_arr = t["depends_on"];
      for (Json::ArrayIndex di = 0; di < deps_arr.size(); di++) {
        if (!deps_arr[di].isString()) continue;
        dep_ids.push_back(trim_copy(deps_arr[di].asString()));
      }
    }
    deps_by_task[task_id] = dep_ids;

    if (infer_depends_on) {
      std::unordered_set<std::string> refs;
      collect_referenced_task_ids_from_json_value(task_req, &refs);
      if (!refs.empty()) {
        bool changed = false;
        std::unordered_set<std::string> have(dep_ids.begin(), dep_ids.end());
        for (const auto& r : refs) {
          if (r.empty() || r == task_id) continue;
          if (have.insert(r).second) {
            dep_ids.push_back(r);
            changed = true;
          }
        }
        if (changed) {
          std::sort(dep_ids.begin(), dep_ids.end());
          dep_ids.erase(std::unique(dep_ids.begin(), dep_ids.end()), dep_ids.end());
          deps_arr = Json::Value(Json::arrayValue);
          for (const auto& d : dep_ids) deps_arr.append(d);
          deps_by_task[task_id] = dep_ids;
        }
      }
    }

    int max_attempts = 1;
	    if (t.isMember("max_attempts") && t["max_attempts"].isInt()) {
	      max_attempts = std::max(1, std::min(512, t["max_attempts"].asInt()));
	    } else if (is_edge) {
      // Edge invocations are inherently async; default to a generous poll budget.
      // (The workflow engine uses `retry_in_ms` for polling delay.)
	      max_attempts = 200;
	    } else if (is_edge_wait_sensor) {
	      // Edge waits are async/polling by design; default to a generous poll budget.
	      max_attempts = 400;
	    }

    const bool allow_error =
      t.isMember("allow_error") && t["allow_error"].isBool() ? t["allow_error"].asBool() : false;

    AgentDb::WorkflowTaskRow row;
    row.workflow_id = workflow_id;
    row.task_id = task_id;
    row.priority = task_priority;
    row.created_unix_ms = now;
    row.updated_unix_ms = now;
    row.status = "queued";
    row.allow_error = allow_error;
    row.attempt = 0;
    row.max_attempts = max_attempts;
    row.ready_unix_ms = 0;
    row.started_unix_ms = 0;
    row.finished_unix_ms = 0;
    row.depends_on_json = deps_arr.isArray() ? json_stringify_compact(deps_arr) : "[]";
    row.request_json = json_stringify_compact(task_req);
    if (t.isMember("expect") && t["expect"].isObject()) {
      row.expect_json = json_stringify_compact(t["expect"]);
    }
    rows.push_back(std::move(row));
    task_ids.push_back(task_id);
  }

  {
    std::string derr;
    if (!validate_dag_or_error(task_ids, deps_by_task, &derr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid workflow DAG";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  // Persist.
  AgentDb::WorkflowRow wf;
  wf.workflow_id = workflow_id;
  wf.session_id = allow_sessions ? session_id : "";
  wf.trace_id = trace_id;
  wf.priority = workflow_priority;
  wf.deadline_unix_ms = args.isMember("deadline_unix_ms") ? args["deadline_unix_ms"].asInt64() : 0;
  wf.idempotency_key = idempotency_key;
  wf.created_unix_ms = now;
  wf.updated_unix_ms = now;
  wf.status = "queued";
  wf.cancel_requested = false;
  wf.error.clear();

  // Store a redacted view of the submit request for troubleshooting.
  {
    Json::Value view = args;
    view["workflow_id"] = workflow_id;
    view["trace_id"] = trace_id;
    view["session_id"] = allow_sessions ? session_id : "";
    view["submitted_unix_ms"] = (Json::Int64)now;
    wf.spec_json = redact_json_best_effort(json_stringify_compact(view));
    if (wf.spec_json.size() > 256 * 1024) {
      wf.spec_json.resize(256 * 1024);
    }
  }

  // If workflows are associated with sessions, ensure the session row exists before inserting the workflow.
  // (workflows.session_id has a foreign key constraint to sessions.session_id)
  if (allow_sessions && !session_id.empty()) {
    std::string sess_err;
    if (!db_or_null->upsert_session(session_id, now, &sess_err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to upsert session";
      o["detail"] = sess_err;
      o["session_id"] = session_id;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  std::string db_err;
  if (!db_or_null->create_workflow(wf, rows, &db_err)) {
    if (!idempotency_key.empty()) {
      const std::string session_scope = allow_sessions ? session_id : "";
      AgentDb::WorkflowRow existing;
      std::string derr;
      const bool found = db_or_null->get_workflow_by_idempotency_key(session_scope, idempotency_key, &existing, &derr);
      if (found && !existing.workflow_id.empty() && derr.empty()) {
        Json::Value o(Json::objectValue);
        o["ok"] = true;
        o["workflow_id"] = existing.workflow_id;
        o["trace_id"] = existing.trace_id;
        if (allow_sessions && !existing.session_id.empty()) o["session_id"] = existing.session_id;
        o["deduped"] = true;
        resp->body = json_stringify_compact(o);
        return;
      }
    }
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to create workflow";
    o["detail"] = db_err;
    o["workflow_id"] = workflow_id;
    resp->body = json_stringify_compact(o);
    return;
  }

  // Workflow event log: initial created marker (best-effort).
  {
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    d["status"] = "queued";
    d["trace_id"] = trace_id;
    if (allow_sessions && !session_id.empty()) d["session_id"] = session_id;
    d["ts_unix_ms"] = (Json::Int64)now;
    AgentDb::WorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_unix_ms = now;
    ev.type = "workflow_created";
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  o["trace_id"] = trace_id;
  if (allow_sessions && !session_id.empty()) o["session_id"] = session_id;
  o["deduped"] = false;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_get_endpoint(
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

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  const auto incl_tasks_q = query_get(req.query, "include_tasks");
  const bool include_tasks = !incl_tasks_q || (*incl_tasks_q != "0" && *incl_tasks_q != "false");
  const auto incl_results_q = query_get(req.query, "include_results");
  const bool include_results = incl_results_q && (*incl_results_q == "1" || *incl_results_q == "true");
  const auto incl_spec_q = query_get(req.query, "include_spec");
  const bool include_spec = incl_spec_q && (*incl_spec_q == "1" || *incl_spec_q == "true");

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(*wid, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value w(Json::objectValue);
  w["workflow_id"] = wf.workflow_id;
  w["status"] = wf.status;
  w["priority"] = wf.priority;
  if (wf.deadline_unix_ms > 0) w["deadline_unix_ms"] = (Json::Int64)wf.deadline_unix_ms;
  if (!wf.idempotency_key.empty()) w["idempotency_key"] = wf.idempotency_key;
  if (!wf.trace_id.empty()) w["trace_id"] = wf.trace_id;
  if (!wf.session_id.empty()) w["session_id"] = wf.session_id;
  w["cancel_requested"] = wf.cancel_requested;
  w["created_unix_ms"] = (Json::Int64)wf.created_unix_ms;
  w["updated_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
  if (!wf.error.empty()) w["error"] = wf.error;
  o["workflow"] = w;

  // Best-effort: surface workflow_limits + aggregate usage/remaining budgets from the persisted spec_json and
  // task cumulative counters (retry-safe).
  {
    Json::Value spec;
    std::string perr;
    if (json_parse_any(wf.spec_json, &spec, &perr) && spec.isObject()) {
      if (spec.isMember("workflow_limits") && spec["workflow_limits"].isObject()) {
        o["workflow_limits"] = spec["workflow_limits"];
      }
    }
  }

  // Best-effort: surface aggregate usage independent of include_tasks so UIs/schedulers can poll cheaply.
  {
    AgentDb::WorkflowUsageTotals totals;
    std::string uerr;
    if (db_or_null->get_workflow_usage_totals(wf.workflow_id, &totals, &uerr)) {
      Json::Value usage(Json::objectValue);
      usage["tool_calls_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.tool_calls_total_used);
      usage["steps_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.steps_total_used);
      usage["elapsed_ms_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.elapsed_ms_total_used);
      usage["prompt_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.prompt_tokens_used);
      usage["completion_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.completion_tokens_used);
      usage["total_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.total_tokens_used);
      o["workflow_usage"] = usage;

      if (o.isMember("workflow_limits") && o["workflow_limits"].isObject()) {
        const Json::Value lim = o["workflow_limits"];
        Json::Value rem(Json::objectValue);

        auto rem_i64 = [&](const char* k, int64_t used) {
          if (!lim.isMember(k)) return;
          const auto& v = lim[k];
          if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return;
          const int64_t maxv = v.asInt64();
          if (maxv <= 0) return;
          rem[k] = (Json::Int64)std::max<int64_t>(0, maxv - std::max<int64_t>(0, used));
        };

        rem_i64("max_tool_calls_total", totals.tool_calls_total_used);
        rem_i64("max_steps_total", totals.steps_total_used);
        rem_i64("max_elapsed_ms_total", totals.elapsed_ms_total_used);
        rem_i64("max_total_tokens", totals.total_tokens_used);

        if (!rem.getMemberNames().empty()) {
          o["workflow_remaining"] = rem;
        }
      }
    }
  }

  if (include_tasks) {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    if (db_or_null->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      Json::Value arr(Json::arrayValue);
      for (const auto& t : tasks) {
        Json::Value row(Json::objectValue);
        row["task_id"] = t.task_id;
        row["status"] = t.status;
        row["allow_error"] = t.allow_error;
        row["priority"] = t.priority;
        row["attempt"] = t.attempt;
        row["max_attempts"] = t.max_attempts;
        row["tool_calls_total_cum"] = (Json::Int64)std::max<int64_t>(0, t.tool_calls_total_cum);
        row["steps_executed_cum"] = (Json::Int64)std::max<int64_t>(0, t.steps_executed_cum);
        row["elapsed_ms_cum"] = (Json::Int64)std::max<int64_t>(0, t.elapsed_ms_cum);
        row["prompt_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.prompt_tokens_cum);
        row["completion_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.completion_tokens_cum);
        row["total_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.total_tokens_cum);

        row["ready_unix_ms"] = (Json::Int64)t.ready_unix_ms;
        row["started_unix_ms"] = (Json::Int64)t.started_unix_ms;
        row["finished_unix_ms"] = (Json::Int64)t.finished_unix_ms;
        if (!t.error.empty()) row["error"] = t.error;
        if (!t.depends_on_json.empty()) {
          Json::Value deps;
          std::string derr;
          if (json_parse_any(t.depends_on_json, &deps, &derr) && deps.isArray()) row["depends_on"] = deps;
        }
        if (include_results && !t.result_json.empty()) {
          Json::Value rr;
          std::string rerr;
          if (json_parse_any(t.result_json, &rr, &rerr)) row["result"] = rr;
        }
        arr.append(row);
      }
      o["tasks"] = arr;
    }
  }

  if (include_results && !wf.result_json.empty()) {
    Json::Value rr;
    std::string rerr;
    if (json_parse_any(wf.result_json, &rr, &rerr)) o["result"] = rr;
  }

  if (include_spec && !wf.spec_json.empty()) {
    // spec_json may have been truncated (size cap), which can render it invalid JSON; return raw string always.
    o["spec_json"] = wf.spec_json;
    Json::Value sr;
    std::string serr;
    if (json_parse_any(wf.spec_json, &sr, &serr)) {
      o["spec"] = sr;
    }
  }

  resp->body = json_stringify_compact(o);
}

void handle_workflow_list_endpoint(
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

  const auto st = query_get(req.query, "status");
  const std::string status = st && !st->empty() ? *st : "running";

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::WorkflowRow> wfs;
  std::string err;
  if (!db_or_null->list_workflows_by_status(status, limit, &wfs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflows";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["status"] = status;
  Json::Value arr(Json::arrayValue);
  for (const auto& wf : wfs) {
    Json::Value row(Json::objectValue);
    row["workflow_id"] = wf.workflow_id;
    row["status"] = wf.status;
    row["priority"] = wf.priority;
    if (wf.deadline_unix_ms > 0) row["deadline_unix_ms"] = (Json::Int64)wf.deadline_unix_ms;
    if (!wf.idempotency_key.empty()) row["idempotency_key"] = wf.idempotency_key;
    if (!wf.trace_id.empty()) row["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) row["session_id"] = wf.session_id;
    row["cancel_requested"] = wf.cancel_requested;
    row["created_unix_ms"] = (Json::Int64)wf.created_unix_ms;
    row["updated_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
    if (!wf.error.empty()) row["error"] = wf.error;
    arr.append(row);
  }
  o["workflows"] = arr;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_cancel_endpoint(
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
    resp->body = json_stringify_compact(o);
    return;
  }

  const std::string workflow_id =
    args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(workflow_id, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  wf.cancel_requested = true;
  wf.updated_unix_ms = unix_ms_now();
  if (wf.status == "queued") wf.status = "running"; // surface cancel in progress
  (void)db_or_null->upsert_workflow(wf, &err);

  // Best-effort: append a cancel_requested event.
  {
    const int64_t now = unix_ms_now();
    Json::Value d(Json::objectValue);
    d["workflow_id"] = wf.workflow_id;
    d["status"] = wf.status;
    d["cancel_requested"] = true;
    if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
    d["ts_unix_ms"] = (Json::Int64)now;
    AgentDb::WorkflowEventRow ev;
    ev.workflow_id = wf.workflow_id;
    ev.ts_unix_ms = now;
    ev.type = "workflow_cancel_requested";
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_events_endpoint(
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

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  int64_t after = 0;
  const auto a = query_get(req.query, "after_event_id");
  if (a && !a->empty()) {
    try { after = (int64_t)std::stoll(*a); } catch (...) { after = 0; }
  }

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::min<size_t>(limit, 1000);

  std::vector<AgentDb::WorkflowEventRow> rows;
  std::string err;
  if (!db_or_null->list_workflow_events(*wid, after, limit, &rows, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflow events";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["workflow_id"] = *wid;
  out["after_event_id"] = (Json::Int64)after;
  Json::Value arr(Json::arrayValue);
  int64_t last = after;
  for (const auto& r : rows) {
    Json::Value o(Json::objectValue);
    o["event_id"] = (Json::Int64)r.event_id;
    o["ts_unix_ms"] = (Json::Int64)r.ts_unix_ms;
    o["type"] = r.type;
    if (!r.task_id.empty()) o["task_id"] = r.task_id;
    Json::Value data;
    std::string perr;
    if (!json_parse_any(r.data_json, &data, &perr)) {
      Json::Value bad(Json::objectValue);
      bad["ok"] = false;
      bad["error"] = "failed to parse event data_json";
      bad["parse_error"] = perr;
      bad["raw"] = r.data_json;
      data = bad;
    }
    o["data"] = data;
    arr.append(o);
    if (r.event_id > last) last = r.event_id;
  }
  out["events"] = arr;
  out["cursor_next"] = (Json::Int64)last;
  resp->body = json_stringify_compact(out);
}

void handle_workflow_stats_endpoint(
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

  AgentDb::WorkflowSchedulerStats st;
  std::string err;
  if (!db_or_null->get_workflow_scheduler_stats(unix_ms_now(), &st, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to read workflow scheduler stats";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["now_unix_ms"] = (Json::Int64)st.now_unix_ms;
  out["tasks_queued_ready"] = (Json::Int64)st.tasks_queued_ready;
  out["tasks_queued_not_ready"] = (Json::Int64)st.tasks_queued_not_ready;
  {
    Json::Value m(Json::objectValue);
    for (const auto& kv : st.workflows_by_status) {
      m[kv.first] = (Json::Int64)kv.second;
    }
    out["workflows_by_status"] = m;
  }
  {
    Json::Value m(Json::objectValue);
    for (const auto& kv : st.tasks_by_status) {
      m[kv.first] = (Json::Int64)kv.second;
    }
    out["tasks_by_status"] = m;
  }

  // Optional: surface aggregate workflow budget pressure (best-effort) so schedulers/UIs can poll cheaply.
  //
  // This is intentionally a bounded scan:
  // - only considers queued|running workflows (active pressure surface)
  // - only includes workflows that define workflow_limits in their persisted spec_json
  // - uses durable per-workflow usage totals derived from retry-safe per-task cumulative counters
  const auto include_budget_q = query_get(req.query, "include_budget_pressure");
  const bool include_budget_pressure = include_budget_q && (*include_budget_q == "1" || *include_budget_q == "true");
  if (include_budget_pressure) {
    size_t max_workflows = 64;
    if (const auto lq = query_get(req.query, "budget_workflow_limit")) {
      try {
        const int n = std::stoi(*lq);
        if (n > 0) max_workflows = (size_t)std::min<int>(512, n);
      } catch (...) {
      }
    }

    const auto include_budget_workflows_q = query_get(req.query, "include_budget_workflows");
    const bool include_budget_workflows =
      include_budget_workflows_q && (*include_budget_workflows_q == "1" || *include_budget_workflows_q == "true");

    auto parse_limits_best_effort = [](const std::string& spec_json) -> Json::Value {
      Json::Value spec;
      std::string perr;
      if (!json_parse_any(spec_json, &spec, &perr) || !spec.isObject()) return Json::Value(Json::nullValue);
      if (!spec.isMember("workflow_limits") || !spec["workflow_limits"].isObject()) return Json::Value(Json::nullValue);
      return spec["workflow_limits"];
    };

    struct LimitAgg {
      int64_t workflows_limited = 0;
      int64_t workflows_remaining_zero = 0;
      int64_t workflows_ratio_le_0_1 = 0;
      int64_t workflows_ratio_le_0_2 = 0;
      int64_t min_remaining = INT64_MAX;
      double min_remaining_ratio = 1.0;
    };
    auto update_limit_agg = [](LimitAgg* agg, int64_t maxv, int64_t used) {
      if (!agg) return;
      if (maxv <= 0) return;
      const int64_t used_clamped = std::max<int64_t>(0, std::min<int64_t>(maxv, std::max<int64_t>(0, used)));
      const int64_t remaining = std::max<int64_t>(0, maxv - used_clamped);
      const double ratio = maxv > 0 ? (double)remaining / (double)maxv : 1.0;
      agg->workflows_limited++;
      if (remaining <= 0) agg->workflows_remaining_zero++;
      if (ratio <= 0.10) agg->workflows_ratio_le_0_1++;
      if (ratio <= 0.20) agg->workflows_ratio_le_0_2++;
      agg->min_remaining = std::min<int64_t>(agg->min_remaining, remaining);
      agg->min_remaining_ratio = std::min<double>(agg->min_remaining_ratio, ratio);
    };

    std::vector<AgentDb::WorkflowRow> wrows;
    wrows.reserve(max_workflows);
    {
      std::vector<AgentDb::WorkflowRow> queued;
      std::vector<AgentDb::WorkflowRow> running;
      std::string qerr, rerr;
      (void)db_or_null->list_workflows_by_status_for_scheduler("queued", max_workflows, &queued, &qerr);
      (void)db_or_null->list_workflows_by_status_for_scheduler("running", max_workflows, &running, &rerr);
      std::unordered_set<std::string> seen;
      for (const auto& r : queued) {
        if (wrows.size() >= max_workflows) break;
        if (r.workflow_id.empty()) continue;
        if (!seen.insert(r.workflow_id).second) continue;
        wrows.push_back(r);
      }
      for (const auto& r : running) {
        if (wrows.size() >= max_workflows) break;
        if (r.workflow_id.empty()) continue;
        if (!seen.insert(r.workflow_id).second) continue;
        wrows.push_back(r);
      }
    }

    int64_t scanned = 0;
    int64_t with_limits = 0;
    LimitAgg tool_calls, steps, elapsed, tokens;
    Json::Value sampled(Json::arrayValue);
    const size_t max_samples = include_budget_workflows ? 12 : 0;

    for (const auto& wf : wrows) {
      scanned++;
      const Json::Value lim = parse_limits_best_effort(wf.spec_json);
      if (!lim.isObject()) continue;
      with_limits++;

      AgentDb::WorkflowUsageTotals totals;
      std::string uerr;
      if (!db_or_null->get_workflow_usage_totals(wf.workflow_id, &totals, &uerr)) {
        continue;
      }

      auto lim_i64 = [&](const char* k, int64_t* outv) {
        if (outv) *outv = 0;
        if (!lim.isMember(k)) return;
        const auto& v = lim[k];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return;
        const int64_t n = v.asInt64();
        if (n <= 0) return;
        if (outv) *outv = n;
      };

      int64_t max_tool_calls = 0, max_steps_total = 0, max_elapsed_ms_total = 0, max_total_tokens = 0;
      lim_i64("max_tool_calls_total", &max_tool_calls);
      lim_i64("max_steps_total", &max_steps_total);
      lim_i64("max_elapsed_ms_total", &max_elapsed_ms_total);
      lim_i64("max_total_tokens", &max_total_tokens);

      update_limit_agg(&tool_calls, max_tool_calls, totals.tool_calls_total_used);
      update_limit_agg(&steps, max_steps_total, totals.steps_total_used);
      update_limit_agg(&elapsed, max_elapsed_ms_total, totals.elapsed_ms_total_used);
      update_limit_agg(&tokens, max_total_tokens, totals.total_tokens_used);

      if (max_samples > 0 && (size_t)sampled.size() < max_samples) {
        Json::Value row(Json::objectValue);
        row["workflow_id"] = wf.workflow_id;
        row["status"] = wf.status;
        if (!wf.session_id.empty()) row["session_id"] = wf.session_id;
        if (!wf.trace_id.empty()) row["trace_id"] = wf.trace_id;
        Json::Value rem(Json::objectValue);
        if (max_tool_calls > 0) rem["max_tool_calls_total"] = (Json::Int64)std::max<int64_t>(0, max_tool_calls - std::max<int64_t>(0, totals.tool_calls_total_used));
        if (max_steps_total > 0) rem["max_steps_total"] = (Json::Int64)std::max<int64_t>(0, max_steps_total - std::max<int64_t>(0, totals.steps_total_used));
        if (max_elapsed_ms_total > 0) rem["max_elapsed_ms_total"] = (Json::Int64)std::max<int64_t>(0, max_elapsed_ms_total - std::max<int64_t>(0, totals.elapsed_ms_total_used));
        if (max_total_tokens > 0) rem["max_total_tokens"] = (Json::Int64)std::max<int64_t>(0, max_total_tokens - std::max<int64_t>(0, totals.total_tokens_used));
        if (!rem.getMemberNames().empty()) row["remaining"] = rem;
        sampled.append(row);
      }
    }

    auto agg_to_json = [](const LimitAgg& a) -> Json::Value {
      Json::Value o(Json::objectValue);
      o["workflows_limited"] = (Json::Int64)std::max<int64_t>(0, a.workflows_limited);
      o["workflows_remaining_zero"] = (Json::Int64)std::max<int64_t>(0, a.workflows_remaining_zero);
      o["workflows_ratio_le_0_1"] = (Json::Int64)std::max<int64_t>(0, a.workflows_ratio_le_0_1);
      o["workflows_ratio_le_0_2"] = (Json::Int64)std::max<int64_t>(0, a.workflows_ratio_le_0_2);
      if (a.workflows_limited > 0) {
        o["min_remaining"] = (Json::Int64)std::max<int64_t>(0, a.min_remaining == INT64_MAX ? 0 : a.min_remaining);
        o["min_remaining_ratio"] = a.min_remaining_ratio;
      }
      return o;
    };

    Json::Value bp(Json::objectValue);
    bp["workflows_scanned"] = (Json::Int64)std::max<int64_t>(0, scanned);
    bp["workflows_with_limits"] = (Json::Int64)std::max<int64_t>(0, with_limits);
    bp["workflow_limit"] = (Json::Int64)max_workflows;
    bp["include_budget_workflows"] = include_budget_workflows;
    bp["tool_calls"] = agg_to_json(tool_calls);
    bp["steps"] = agg_to_json(steps);
    bp["elapsed_ms"] = agg_to_json(elapsed);
    bp["total_tokens"] = agg_to_json(tokens);
    if (include_budget_workflows) bp["workflows"] = sampled;
    out["budget_pressure"] = bp;
  }

  // Optional: session-level inflight pressure snapshot (multi-tenant / fairness tuning).
  const auto include_sessions_q = query_get(req.query, "include_sessions");
  const bool include_sessions = include_sessions_q && (*include_sessions_q == "1" || *include_sessions_q == "true");
  if (include_sessions) {
    size_t limit = 32;
    if (const auto lq = query_get(req.query, "session_limit")) {
      try {
        const int n = std::stoi(*lq);
        if (n > 0) limit = (size_t)std::min<int>(512, n);
      } catch (...) {
      }
    }
    const auto include_no_session_q = query_get(req.query, "include_no_session");
    const bool include_no_session =
      include_no_session_q && (*include_no_session_q == "1" || *include_no_session_q == "true");

    std::vector<AgentDb::WorkflowSessionStatsRow> rows;
    std::string serr;
    if (db_or_null->list_workflow_session_stats(limit, include_no_session, &rows, &serr)) {
      Json::Value arr(Json::arrayValue);
      for (const auto& r : rows) {
        Json::Value o(Json::objectValue);
        o["session_id"] = r.session_id;
        o["inflight_tasks"] = (Json::Int64)std::max<int64_t>(0, r.inflight_tasks);
        o["queued_tasks"] = (Json::Int64)std::max<int64_t>(0, r.queued_tasks);
        o["running_tasks"] = (Json::Int64)std::max<int64_t>(0, r.running_tasks);
        o["workflows_queued"] = (Json::Int64)std::max<int64_t>(0, r.workflows_queued);
        o["workflows_running"] = (Json::Int64)std::max<int64_t>(0, r.workflows_running);
        arr.append(o);
      }
      out["session_limit"] = (Json::Int64)limit;
      out["include_no_session"] = include_no_session;
      out["sessions"] = arr;
    } else {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = serr;
      out["sessions_error"] = o;
    }
  }
  resp->body = json_stringify_compact(out);
}

}  // namespace agentd
