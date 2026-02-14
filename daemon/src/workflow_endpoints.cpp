#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "drain_state.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "string_util.h"
#include "workflow_submit_macros.h"
#include "workflow_submit_task_builders.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <functional>
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

static bool parse_citation_path_line(const std::string& citation, std::string* out_path, int* out_line) {
  if (out_path) out_path->clear();
  if (out_line) *out_line = 0;
  const std::string c = trim_copy(citation);
  if (c.empty()) return false;
  const size_t pos = c.rfind(':');
  if (pos == std::string::npos) return false;
  const std::string path = trim_copy(c.substr(0, pos));
  const std::string line_str = trim_copy(c.substr(pos + 1));
  if (path.empty() || line_str.empty()) return false;
  int line = 0;
  try {
    line = std::stoi(line_str);
  } catch (...) {
    return false;
  }
  if (line <= 0) return false;
  if (out_path) *out_path = path;
  if (out_line) *out_line = line;
  return true;
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
        const std::string kl = lower_copy(k);
        if (kl == "api_key" || kl == "authorization" || kl == "auth_token") {
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
  if (drain_is_active()) {
    resp->status = 503;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "draining";
    const int64_t until_ms = drain_until_unix_ms();
    if (until_ms > 0) o["drain_until_unix_ms"] = (Json::Int64)until_ms;
    const std::string reason = drain_reason();
    if (!reason.empty()) o["drain_reason"] = reason;
    resp->body = json_stringify_compact(o);
    return;
  }

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
	    const bool is_memory_timeline = (kind == "memory_timeline");
	    const bool is_memory_structured_query = (kind == "memory_structured_query");
	    const bool is_memory_correlate = (kind == "memory_correlate");
	    const bool is_memory_query = (kind == "memory_query");
	    const bool is_memory_consolidate = (kind == "memory_consolidate");
	    const bool is_http_json = (kind == "http_json");
	    const bool is_agentd_call = (kind == "agentd_call");
	    const bool is_special =
	      is_avm || is_aggregate || is_edge || is_edge_wait_sensor || is_delay || is_delegate || is_memory_put || is_memory_search ||
	      is_memory_timeline || is_memory_structured_query || is_memory_correlate || is_memory_query || is_memory_consolidate || is_http_json || is_agentd_call;

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
	    } else if (is_memory_timeline) {
	      if (!t.isMember("memory_timeline") || !t["memory_timeline"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_timeline task missing memory_timeline object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      const auto& mt = t["memory_timeline"];
	      Json::Value mt2(Json::objectValue);

	      std::string rel_path;
	      int line = 0;
	      const std::string citation_raw =
	        mt.isMember("citation") && mt["citation"].isString() ? trim_copy(mt["citation"].asString()) : "";
	      const bool citation_templated = citation_raw.find("${") != std::string::npos;
	      if (!citation_raw.empty() && !citation_templated) {
	        if (!parse_citation_path_line(citation_raw, &rel_path, &line)) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_timeline.citation must be path:line";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	      } else if (citation_raw.empty()) {
	        rel_path = mt.isMember("path") && mt["path"].isString() ? trim_copy(mt["path"].asString()) : "";
	        line = mt.isMember("line") && mt["line"].isInt() ? mt["line"].asInt() : 0;
	      } else {
	        // templated citation; defer validation to runtime
	        mt2["citation"] = citation_raw;
	      }
	      if (citation_raw.empty()) {
	        if (!is_safe_relpath_md(rel_path) || line <= 0) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_timeline requires path+line (safe .md path) or citation path:line";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	      } else if (!citation_templated) {
	        if (!is_safe_relpath_md(rel_path) || line <= 0) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_timeline requires a safe .md citation";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	      }

	      if (mt.isMember("context_lines") && !mt["context_lines"].isInt() && !mt["context_lines"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_timeline.context_lines must be an int";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (mt.isMember("max_chars") && !mt["max_chars"].isInt() && !mt["max_chars"].isNull()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_timeline.max_chars must be an int";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }

	      if (!citation_templated) {
	        mt2["path"] = rel_path;
	        mt2["line"] = line;
	        mt2["citation"] = rel_path + ":" + std::to_string(line);
	      }
	      if (mt.isMember("context_lines") && mt["context_lines"].isInt()) {
	        mt2["context_lines"] = std::max(0, std::min(50, mt["context_lines"].asInt()));
	      }
	      if (mt.isMember("max_chars") && mt["max_chars"].isInt()) {
	        const int v = mt["max_chars"].asInt();
	        mt2["max_chars"] = std::max(200, std::min(10000, v));
	      }

	      task_req["kind"] = "memory_timeline";
	      task_req["memory_timeline"] = mt2;
	      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_structured_query) {
	      if (!t.isMember("memory_structured_query") || !t["memory_structured_query"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query task missing memory_structured_query object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      const auto& msq = t["memory_structured_query"];
	      Json::Value msq2(Json::objectValue);

	      const std::string path =
	        msq.isMember("path") && msq["path"].isString() ? trim_copy(msq["path"].asString()) : "STRUCTURED.md";
	      if (!is_safe_relpath_md(path)) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query.path must be a safe relative .md path (default: STRUCTURED.md)";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      msq2["path"] = path;

	      const std::string key = msq.isMember("key") && msq["key"].isString() ? trim_copy(msq["key"].asString()) : "";
	      if (!key.empty() && key.size() > 200) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query.key too long (max 200 chars)";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (!key.empty()) msq2["key"] = key;

	      const std::string key_prefix =
	        msq.isMember("key_prefix") && msq["key_prefix"].isString() ? trim_copy(msq["key_prefix"].asString()) : "";
	      if (!key_prefix.empty() && key_prefix.size() > 200) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query.key_prefix too long (max 200 chars)";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (!key_prefix.empty()) msq2["key_prefix"] = key_prefix;

	      if (msq.isMember("key_case_insensitive")) {
	        if (!msq["key_case_insensitive"].isBool() && !msq["key_case_insensitive"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.key_case_insensitive must be a bool";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["key_case_insensitive"].isBool()) msq2["key_case_insensitive"] = msq["key_case_insensitive"];
	      }

	      Json::Value kinds2(Json::arrayValue);
	      if (msq.isMember("kinds")) {
	        if (!msq["kinds"].isArray() && !msq["kinds"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.kinds must be an array";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["kinds"].isArray()) {
	          for (Json::ArrayIndex i = 0; i < msq["kinds"].size(); i++) {
	            if (!msq["kinds"][i].isString()) continue;
	            const std::string k = lower_copy(trim_copy(msq["kinds"][i].asString()));
	            if (k.empty()) continue;
	            kinds2.append(k == "pref" ? "preference" : k);
	          }
	        }
	      }
	      if (!kinds2.empty()) msq2["kinds"] = kinds2;

	      const std::string source_contains =
	        msq.isMember("source_contains") && msq["source_contains"].isString() ? trim_copy(msq["source_contains"].asString()) : "";
	      if (!source_contains.empty() && source_contains.size() > 300) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query.source_contains too long (max 300 chars)";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	      if (!source_contains.empty()) msq2["source_contains"] = source_contains;

	      if (msq.isMember("source_case_insensitive")) {
	        if (!msq["source_case_insensitive"].isBool() && !msq["source_case_insensitive"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.source_case_insensitive must be a bool";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["source_case_insensitive"].isBool()) msq2["source_case_insensitive"] = msq["source_case_insensitive"];
	      }

	      if (msq.isMember("updated_since_utc")) {
	        if (!msq["updated_since_utc"].isString() && !msq["updated_since_utc"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.updated_since_utc must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["updated_since_utc"].isString() && !trim_copy(msq["updated_since_utc"].asString()).empty()) {
	          msq2["updated_since_utc"] = trim_copy(msq["updated_since_utc"].asString());
	        }
	      }
	      if (msq.isMember("updated_until_utc")) {
	        if (!msq["updated_until_utc"].isString() && !msq["updated_until_utc"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.updated_until_utc must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["updated_until_utc"].isString() && !trim_copy(msq["updated_until_utc"].asString()).empty()) {
	          msq2["updated_until_utc"] = trim_copy(msq["updated_until_utc"].asString());
	        }
	      }

	      if (msq.isMember("order_by")) {
	        if (!msq["order_by"].isString() && !msq["order_by"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.order_by must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["order_by"].isString() && !trim_copy(msq["order_by"].asString()).empty()) {
	          msq2["order_by"] = trim_copy(msq["order_by"].asString());
	        }
	      }

	      // Safety: avoid accidentally dumping the entire structured memory file by default.
	      if (!msq2.isMember("key") && !msq2.isMember("key_prefix") &&
	          (!msq2.isMember("kinds") || !msq2["kinds"].isArray() || msq2["kinds"].empty()) &&
	          !msq2.isMember("source_contains")) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_structured_query requires at least one filter: key, key_prefix, non-empty kinds[], or source_contains";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }

	      if (msq.isMember("status")) {
	        if (!msq["status"].isString() && !msq["status"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.status must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["status"].isString() && !trim_copy(msq["status"].asString()).empty()) msq2["status"] = trim_copy(msq["status"].asString());
	      }
	      if (msq.isMember("include_sources")) {
	        if (!msq["include_sources"].isBool() && !msq["include_sources"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.include_sources must be a bool";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["include_sources"].isBool()) msq2["include_sources"] = msq["include_sources"];
	      }
	      if (msq.isMember("include_versions")) {
	        if (!msq["include_versions"].isBool() && !msq["include_versions"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.include_versions must be a bool";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["include_versions"].isBool()) msq2["include_versions"] = msq["include_versions"];
	      }
	      if (msq.isMember("limit")) {
	        if (!msq["limit"].isInt() && !msq["limit"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_structured_query.limit must be an int";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (msq["limit"].isInt()) msq2["limit"] = std::max(1, std::min(200, msq["limit"].asInt()));
	      }

	      task_req["kind"] = "memory_structured_query";
	      task_req["memory_structured_query"] = msq2;
	      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_correlate) {
	      if (!t.isMember("memory_correlate") || !t["memory_correlate"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_correlate task missing memory_correlate object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }

	      const auto& mc = t["memory_correlate"];
	      Json::Value mc2(Json::objectValue);

	      if (mc.isMember("trace_id")) {
	        if (!mc["trace_id"].isString() && !mc["trace_id"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_correlate.trace_id must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mc["trace_id"].isString() && !trim_copy(mc["trace_id"].asString()).empty()) {
	          mc2["trace_id"] = trim_copy(mc["trace_id"].asString());
	        }
	      }

	      if (mc.isMember("since_utc_ms")) {
	        if (!(mc["since_utc_ms"].isInt64() || mc["since_utc_ms"].isUInt64() || mc["since_utc_ms"].isInt() || mc["since_utc_ms"].isUInt()) && !mc["since_utc_ms"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_correlate.since_utc_ms must be an int64";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (!mc["since_utc_ms"].isNull()) mc2["since_utc_ms"] = (Json::Int64)mc["since_utc_ms"].asInt64();
	      }

	      if (mc.isMember("until_utc_ms")) {
	        if (!(mc["until_utc_ms"].isInt64() || mc["until_utc_ms"].isUInt64() || mc["until_utc_ms"].isInt() || mc["until_utc_ms"].isUInt()) && !mc["until_utc_ms"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_correlate.until_utc_ms must be an int64";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (!mc["until_utc_ms"].isNull()) mc2["until_utc_ms"] = (Json::Int64)mc["until_utc_ms"].asInt64();
	      }

	      if (mc.isMember("max_entries")) {
	        if (!mc["max_entries"].isInt() && !mc["max_entries"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_correlate.max_entries must be an int";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mc["max_entries"].isInt()) {
	          mc2["max_entries"] = std::max(1, std::min(500, mc["max_entries"].asInt()));
	        }
	      }

	      if (mc.isMember("timeline")) {
	        if (!mc["timeline"].isBool() && !mc["timeline"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_correlate.timeline must be a bool";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mc["timeline"].isBool()) mc2["timeline"] = mc["timeline"];
	      }

	      task_req["kind"] = "memory_correlate";
	      task_req["memory_correlate"] = mc2;
	      task_req["priority"] = task_priority;
	      task_req["trace_id"] = trace_id + ":" + task_id;
	    } else if (is_memory_query) {
	      if (!t.isMember("memory_query") || !t["memory_query"].isObject()) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = "memory_query task missing memory_query object";
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }

	      const auto& mq = t["memory_query"];
	      Json::Value mq2(Json::objectValue);

	      if (mq.isMember("since_utc_ms")) {
	        if (!(mq["since_utc_ms"].isInt64() || mq["since_utc_ms"].isUInt64() || mq["since_utc_ms"].isInt() || mq["since_utc_ms"].isUInt()) && !mq["since_utc_ms"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_query.since_utc_ms must be an int64";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (!mq["since_utc_ms"].isNull()) mq2["since_utc_ms"] = (Json::Int64)mq["since_utc_ms"].asInt64();
	      }

	      if (mq.isMember("until_utc_ms")) {
	        if (!(mq["until_utc_ms"].isInt64() || mq["until_utc_ms"].isUInt64() || mq["until_utc_ms"].isInt() || mq["until_utc_ms"].isUInt()) && !mq["until_utc_ms"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_query.until_utc_ms must be an int64";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (!mq["until_utc_ms"].isNull()) mq2["until_utc_ms"] = (Json::Int64)mq["until_utc_ms"].asInt64();
	      }

	      if (mq.isMember("structured_path")) {
	        if (!mq["structured_path"].isString() && !mq["structured_path"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_query.structured_path must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mq["structured_path"].isString() && !trim_copy(mq["structured_path"].asString()).empty()) {
	          mq2["structured_path"] = trim_copy(mq["structured_path"].asString());
	        }
	      }

	      if (mq.isMember("key_prefix")) {
	        if (!mq["key_prefix"].isString() && !mq["key_prefix"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_query.key_prefix must be a string";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mq["key_prefix"].isString()) mq2["key_prefix"] = mq["key_prefix"];
	      }

	      if (mq.isMember("limit")) {
	        if (!mq["limit"].isInt() && !mq["limit"].isNull()) {
	          resp->status = 400;
	          Json::Value o(Json::objectValue);
	          o["ok"] = false;
	          o["error"] = "memory_query.limit must be an int";
	          o["task_id"] = task_id;
	          resp->body = json_stringify_compact(o);
	          return;
	        }
	        if (mq["limit"].isInt()) {
	          mq2["limit"] = std::max(1, std::min(1000, mq["limit"].asInt()));
	        }
	      }

	      task_req["kind"] = "memory_query";
	      task_req["memory_query"] = mq2;
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
	    } else if (is_http_json) {
	      std::string berr;
	      if (!workflow_submit_build_http_json_task_request(t, task_id, task_priority, trace_id, &task_req, &berr)) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = berr.empty() ? "invalid http_json task" : berr;
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
	    } else if (is_agentd_call) {
	      std::string berr;
	      if (!workflow_submit_build_agentd_call_task_request(t, task_id, task_priority, trace_id, &task_req, &berr)) {
	        resp->status = 400;
	        Json::Value o(Json::objectValue);
	        o["ok"] = false;
	        o["error"] = berr.empty() ? "invalid agentd_call task" : berr;
	        o["task_id"] = task_id;
	        resp->body = json_stringify_compact(o);
	        return;
	      }
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

}  // namespace agentd
