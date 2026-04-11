#include "workflow_submit_task_builders.h"

#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

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

static bool set_header_checked(
  Json::Value* headers,
  const std::string& key,
  const std::string& value,
  std::string* out_error
) {
  if (!headers || key.empty()) return false;
  if (!headers->isObject()) *headers = Json::Value(Json::objectValue);
  const std::string want = lower_copy(key);
  for (const auto& k : headers->getMemberNames()) {
    if (lower_copy(k) != want) continue;
    if (!(*headers)[k].isString() || trim_copy((*headers)[k].asString()) != value) {
      if (out_error) *out_error = "conflicting " + key + " header";
      return false;
    }
    return true;
  }
  (*headers)[key] = value;
  return true;
}

static bool env_name_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  const auto is_alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
  const auto is_num = [](char c) { return (c >= '0' && c <= '9'); };
  if (!(is_alpha(s[0]) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(is_alpha(c) || is_num(c) || c == '_')) return false;
  }
  return true;
}

}  // namespace

bool workflow_submit_build_http_json_task_request(
  const Json::Value& task_spec,
  const std::string& task_id,
  int task_priority,
  const std::string& trace_id,
  Json::Value* out_task_req,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_task_req) return false;
  *out_task_req = Json::Value(Json::objectValue);

  if (!task_spec.isMember("http_json") || !task_spec["http_json"].isObject()) {
    if (out_error) *out_error = "http_json task missing http_json object";
    return false;
  }
  const auto& hj = task_spec["http_json"];
  const std::string url =
    hj.isMember("url") && hj["url"].isString() ? trim_copy(hj["url"].asString()) : "";
  if (url.empty()) {
    if (out_error) *out_error = "http_json.url must be a non-empty string";
    return false;
  }
  if (!(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
    if (out_error) *out_error = "http_json.url must start with http:// or https://";
    return false;
  }
  if (url.size() > 4096) {
    if (out_error) *out_error = "http_json.url is too long";
    return false;
  }

  std::string method =
    hj.isMember("method") && hj["method"].isString() ? trim_copy(hj["method"].asString()) : "POST";
  for (char& c : method) c = (char)std::toupper((unsigned char)c);
  if (!(method == "GET" || method == "POST")) {
    if (out_error) *out_error = "http_json.method must be GET or POST";
    return false;
  }

  const bool has_body = hj.isMember("body") && !hj["body"].isNull();
  if (method == "GET" && has_body) {
    if (out_error) *out_error = "http_json.body is not allowed for method=GET";
    return false;
  }

  Json::Value headers2(Json::objectValue);
  if (hj.isMember("headers")) {
    if (!hj["headers"].isObject() && !hj["headers"].isNull()) {
      if (out_error) *out_error = "http_json.headers must be an object";
      return false;
    }
    if (hj["headers"].isObject()) {
      const auto& hdr = hj["headers"];
      int kept = 0;
      for (const auto& k : hdr.getMemberNames()) {
        if (kept >= 32) break;
        if (!hdr[k].isString()) continue;
        const std::string key = trim_copy(k);
        if (key.empty() || key.size() > 64) continue;
        // Only allow simple token header names (A-Za-z0-9-), to prevent injection/CRLF ambiguity.
        bool ok = true;
        for (char c : key) {
          const bool is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
          if (!(is_alnum || c == '-')) {
            ok = false;
            break;
          }
        }
        if (!ok) continue;
        const std::string kl = lower_copy(key);
        if (kl == "authorization" || kl == "proxy-authorization") {
          if (out_error) *out_error = "http_json.headers must not include Authorization; use bearer_env instead (prevents persisting secrets)";
          return false;
        }
        const std::string val = hdr[k].asString();
        if (val.size() > 4096) {
          if (out_error) *out_error = "http_json.headers values are too long (max 4096 chars)";
          return false;
        }
        headers2[key] = val;
        kept++;
      }
    }
  }

  Json::Value hj2(Json::objectValue);
  hj2["url"] = url;
  hj2["method"] = method;
  if (!headers2.empty()) hj2["headers"] = headers2;
  if (has_body) {
    // Bound persisted body size to keep specs lean.
    const std::string body_s = json_stringify_compact(hj["body"]);
    if (body_s.size() > 256 * 1024) {
      if (out_error) *out_error = "http_json.body is too large (max 256KiB when JSON-stringified)";
      return false;
    }
    hj2["body"] = hj["body"];
  }

  if (hj.isMember("bearer_env")) {
    if (!hj["bearer_env"].isString() && !hj["bearer_env"].isNull()) {
      if (out_error) *out_error = "http_json.bearer_env must be a string";
      return false;
    }
    if (hj["bearer_env"].isString()) {
      const std::string env = trim_copy(hj["bearer_env"].asString());
      if (!env.empty()) {
        if (!env_name_is_safe(env)) {
          if (out_error) *out_error = "http_json.bearer_env must be a safe env var name";
          return false;
        }
        hj2["bearer_env"] = env;
      }
    }
  }

  int64_t timeout_ms = 30000;
  if (hj.isMember("timeout_ms")) {
    if (!(hj["timeout_ms"].isInt64() || hj["timeout_ms"].isUInt64() || hj["timeout_ms"].isInt() || hj["timeout_ms"].isUInt()) && !hj["timeout_ms"].isNull()) {
      if (out_error) *out_error = "http_json.timeout_ms must be an int64";
      return false;
    }
    if (!hj["timeout_ms"].isNull()) timeout_ms = hj["timeout_ms"].asInt64();
  }
  if (timeout_ms < 1) timeout_ms = 1;
  if (timeout_ms > 300000) timeout_ms = 300000;
  hj2["timeout_ms"] = (Json::Int64)timeout_ms;

  int64_t max_bytes = 1024 * 1024;
  if (hj.isMember("max_bytes")) {
    if (!(hj["max_bytes"].isInt64() || hj["max_bytes"].isUInt64() || hj["max_bytes"].isInt() || hj["max_bytes"].isUInt()) && !hj["max_bytes"].isNull()) {
      if (out_error) *out_error = "http_json.max_bytes must be an int64";
      return false;
    }
    if (!hj["max_bytes"].isNull()) max_bytes = hj["max_bytes"].asInt64();
  }
  if (max_bytes < 1024) max_bytes = 1024;
  if (max_bytes > 16LL * 1024LL * 1024LL) max_bytes = 16LL * 1024LL * 1024LL;
  hj2["max_bytes"] = (Json::Int64)max_bytes;

  (*out_task_req)["kind"] = "http_json";
  (*out_task_req)["http_json"] = hj2;
  (*out_task_req)["priority"] = task_priority;
  (*out_task_req)["trace_id"] = trace_id + ":" + task_id;
  return true;
}

bool workflow_submit_build_agentd_call_task_request(
  const Json::Value& task_spec,
  const std::string& task_id,
  int task_priority,
  const std::string& trace_id,
  Json::Value* out_task_req,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_task_req) return false;
  *out_task_req = Json::Value(Json::objectValue);

  if (!task_spec.isMember("agentd_call") || !task_spec["agentd_call"].isObject()) {
    if (out_error) *out_error = "agentd_call task missing agentd_call object";
    return false;
  }
  const auto& ac = task_spec["agentd_call"];
  std::string base_url =
    ac.isMember("base_url") && ac["base_url"].isString() ? trim_copy(ac["base_url"].asString()) : "";
  std::string broker_agent_id =
    ac.isMember("broker_agent_id") && ac["broker_agent_id"].isString() ? trim_copy(ac["broker_agent_id"].asString()) : "";
  std::string broker_deployment_id =
    ac.isMember("broker_deployment_id") && ac["broker_deployment_id"].isString() ? trim_copy(ac["broker_deployment_id"].asString()) : "";
  std::string target_id =
    ac.isMember("target_id") && ac["target_id"].isString() ? trim_copy(ac["target_id"].asString()) : "";
  std::string target_identity =
    ac.isMember("target_identity") && ac["target_identity"].isString() ? trim_copy(ac["target_identity"].asString()) : "";
  if (base_url.empty()) {
    // Optional convenience: compute base_url from broker proxy fields.
    const Json::Value bp = ac.isMember("broker_proxy") ? ac["broker_proxy"] : Json::Value(Json::nullValue);
    if (!bp.isNull()) {
      if (!bp.isObject()) {
        if (out_error) *out_error = "agentd_call.broker_proxy must be an object";
        return false;
      }
      const std::string broker_base_url =
        bp.isMember("broker_base_url") && bp["broker_base_url"].isString() ? trim_copy(bp["broker_base_url"].asString()) : "";
      const std::string agent_id =
        bp.isMember("agent_id") && bp["agent_id"].isString() ? trim_copy(bp["agent_id"].asString()) : "";
      const std::string deployment_id =
        bp.isMember("deployment_id") && bp["deployment_id"].isString() ? trim_copy(bp["deployment_id"].asString()) : "";
      if (broker_base_url.empty()) {
        if (out_error) *out_error = "agentd_call.broker_proxy.broker_base_url must be a non-empty string";
        return false;
      }
      if (!(broker_base_url.rfind("http://", 0) == 0 || broker_base_url.rfind("https://", 0) == 0)) {
        if (out_error) *out_error = "agentd_call.broker_proxy.broker_base_url must start with http:// or https://";
        return false;
      }
      if (broker_base_url.size() > 4096) {
        if (out_error) *out_error = "agentd_call.broker_proxy.broker_base_url is too long";
        return false;
      }
      if (agent_id.empty() || !id_is_safe(agent_id)) {
        if (out_error) *out_error = "agentd_call.broker_proxy.agent_id must be id-safe";
        return false;
      }
      if (!deployment_id.empty() && !id_is_safe(deployment_id)) {
        if (out_error) *out_error = "agentd_call.broker_proxy.deployment_id must be id-safe";
        return false;
      }
      base_url = join_base_path(broker_base_url, "/v1/agents/" + agent_id + "/proxy");
      broker_agent_id = agent_id;
      broker_deployment_id = deployment_id;
      target_identity = "broker:" + broker_base_url + ":" + agent_id;
      if (!deployment_id.empty()) target_identity += ":" + deployment_id;
    }
  }
  if (base_url.empty()) {
    if (out_error) *out_error = "agentd_call.base_url is required (or set agentd_call.broker_proxy)";
    return false;
  }
  if (!(base_url.rfind("http://", 0) == 0 || base_url.rfind("https://", 0) == 0)) {
    if (out_error) *out_error = "agentd_call.base_url must start with http:// or https://";
    return false;
  }
  if (base_url.size() > 4096) {
    if (out_error) *out_error = "agentd_call.base_url is too long";
    return false;
  }
  if (target_identity.empty()) target_identity = "url:" + base_url;
  if (!target_id.empty() && !id_is_safe(target_id)) {
    if (out_error) *out_error = "agentd_call.target_id must be id-safe";
    return false;
  }
  if (!broker_agent_id.empty() && !id_is_safe(broker_agent_id)) {
    if (out_error) *out_error = "agentd_call.broker_agent_id must be id-safe";
    return false;
  }
  if (!broker_deployment_id.empty() && !id_is_safe(broker_deployment_id)) {
    if (out_error) *out_error = "agentd_call.broker_deployment_id must be id-safe";
    return false;
  }
  if (target_identity.size() > 4096) {
    if (out_error) *out_error = "agentd_call.target_identity is too long";
    return false;
  }

  const std::string op =
    ac.isMember("op") && ac["op"].isString() ? trim_copy(ac["op"].asString()) : "workflow_submit_and_wait";
  if (op != "workflow_submit_and_wait") {
    if (out_error) *out_error = "agentd_call.op must be workflow_submit_and_wait";
    return false;
  }

  if (!ac.isMember("workflow") || !ac["workflow"].isObject()) {
    if (out_error) *out_error = "agentd_call.workflow must be an object";
    return false;
  }

  // Persisted agentd_call is kept minimal; most validation is remote-side.
  // Still enforce basic header hygiene: do not persist Authorization.
  Json::Value headers2(Json::objectValue);
  if (ac.isMember("headers")) {
    if (!ac["headers"].isObject() && !ac["headers"].isNull()) {
      if (out_error) *out_error = "agentd_call.headers must be an object";
      return false;
    }
    if (ac["headers"].isObject()) {
      const auto& hdr = ac["headers"];
      int kept = 0;
      for (const auto& k : hdr.getMemberNames()) {
        if (kept >= 32) break;
        if (!hdr[k].isString()) continue;
        const std::string key = trim_copy(k);
        if (key.empty() || key.size() > 64) continue;
        bool ok = true;
        for (char c : key) {
          const bool is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
          if (!(is_alnum || c == '-')) { ok = false; break; }
        }
        if (!ok) continue;
        const std::string kl = lower_copy(key);
        if (kl == "authorization" || kl == "proxy-authorization") {
          if (out_error) *out_error = "agentd_call.headers must not include Authorization; use bearer_env instead (prevents persisting secrets)";
          return false;
        }
        const std::string val = hdr[k].asString();
        if (val.size() > 4096) {
          if (out_error) *out_error = "agentd_call.headers values are too long (max 4096 chars)";
          return false;
        }
        headers2[key] = val;
        kept++;
      }
    }
  }
  if (!broker_deployment_id.empty()) {
    if (!set_header_checked(&headers2, "X-Agentd-Deployment", broker_deployment_id, out_error)) {
      return false;
    }
  }

  Json::Value ac2(Json::objectValue);
  ac2["base_url"] = base_url;
  ac2["op"] = op;
  ac2["workflow"] = ac["workflow"];
  if (!headers2.empty()) ac2["headers"] = headers2;
  if (!target_id.empty()) ac2["target_id"] = target_id;
  if (!target_identity.empty()) ac2["target_identity"] = target_identity;
  if (!broker_agent_id.empty()) ac2["broker_agent_id"] = broker_agent_id;
  if (!broker_deployment_id.empty()) ac2["broker_deployment_id"] = broker_deployment_id;

  if (ac.isMember("bearer_env")) {
    if (!ac["bearer_env"].isString() && !ac["bearer_env"].isNull()) {
      if (out_error) *out_error = "agentd_call.bearer_env must be a string";
      return false;
    }
    if (ac["bearer_env"].isString()) {
      const std::string env = trim_copy(ac["bearer_env"].asString());
      if (!env.empty()) {
        if (!env_name_is_safe(env)) {
          if (out_error) *out_error = "agentd_call.bearer_env must be a safe env var name";
          return false;
        }
        ac2["bearer_env"] = env;
      }
    }
  }

  auto copy_i64_opt = [&](const char* k, int64_t lo, int64_t hi) -> bool {
    if (!ac.isMember(k)) return true;
    if (!(ac[k].isInt64() || ac[k].isUInt64() || ac[k].isInt() || ac[k].isUInt()) && !ac[k].isNull()) {
      if (out_error) *out_error = std::string("agentd_call.") + k + " must be an int64";
      return false;
    }
    if (!ac[k].isNull()) {
      int64_t v = ac[k].asInt64();
      if (v < lo) v = lo;
      if (v > hi) v = hi;
      ac2[k] = (Json::Int64)v;
    }
    return true;
  };

  if (!copy_i64_opt("timeout_ms", 1, 300000)) return false;
  if (!copy_i64_opt("poll_ms", 10, 1000)) return false;
  if (!copy_i64_opt("max_bytes", 1024, 16LL * 1024LL * 1024LL)) return false;

  if (ac.isMember("include_tasks")) {
    if (!ac["include_tasks"].isBool() && !ac["include_tasks"].isNull()) {
      if (out_error) *out_error = "agentd_call.include_tasks must be a bool";
      return false;
    }
    if (ac["include_tasks"].isBool()) ac2["include_tasks"] = ac["include_tasks"];
  }
  if (ac.isMember("include_results")) {
    if (!ac["include_results"].isBool() && !ac["include_results"].isNull()) {
      if (out_error) *out_error = "agentd_call.include_results must be a bool";
      return false;
    }
    if (ac["include_results"].isBool()) ac2["include_results"] = ac["include_results"];
  }

  (*out_task_req)["kind"] = "agentd_call";
  (*out_task_req)["agentd_call"] = ac2;
  (*out_task_req)["priority"] = task_priority;
  (*out_task_req)["trace_id"] = trace_id + ":" + task_id;
  return true;
}

bool workflow_submit_build_memory_consolidate_task_request(
  const Json::Value& task_spec,
  const std::string& task_id,
  int task_priority,
  const std::string& trace_id,
  Json::Value* out_task_req,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_task_req) return false;
  *out_task_req = Json::Value(Json::objectValue);

  if (task_spec.isMember("memory_consolidate") &&
      !task_spec["memory_consolidate"].isObject() &&
      !task_spec["memory_consolidate"].isNull()) {
    if (out_error) *out_error = "memory_consolidate must be an object";
    return false;
  }

  const Json::Value mc =
    task_spec.isMember("memory_consolidate") && task_spec["memory_consolidate"].isObject()
      ? task_spec["memory_consolidate"]
      : Json::Value(Json::objectValue);
  Json::Value mc2(Json::objectValue);

  auto copy_int = [&](const char* field, int min_value) -> bool {
    if (!mc.isMember(field)) return true;
    if (!mc[field].isInt()) {
      if (out_error) *out_error = std::string("memory_consolidate.") + field + " must be an int";
      return false;
    }
    mc2[field] = std::max(min_value, mc[field].asInt());
    return true;
  };
  if (!copy_int("daily_days", 0)) return false;
  if (!copy_int("session_days", 0)) return false;
  if (!copy_int("keep_checkpoints", 1)) return false;
  if (!copy_int("max_entries", 1)) return false;
  if (!copy_int("max_file_bytes", 1024)) return false;
  if (!copy_int("max_session_files", 0)) return false;

  if (mc.isMember("dry_run")) {
    if (!mc["dry_run"].isBool()) {
      if (out_error) *out_error = "memory_consolidate.dry_run must be a bool";
      return false;
    }
    mc2["dry_run"] = mc["dry_run"];
  }
  for (const char* field : {"include_core", "include_daily", "include_session", "include_structured"}) {
    if (!mc.isMember(field)) continue;
    if (!mc[field].isBool()) {
      if (out_error) *out_error = std::string("memory_consolidate.") + field + " must be a bool";
      return false;
    }
    mc2[field] = mc[field];
  }

  (*out_task_req)["kind"] = "memory_consolidate";
  (*out_task_req)["memory_consolidate"] = mc2;
  (*out_task_req)["priority"] = task_priority;
  (*out_task_req)["trace_id"] = trace_id + ":" + task_id;
  return true;
}

}  // namespace agentd
