#include "workflow_http_json.h"

#include "http_client.h"
#include "http_allowlist.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace agentd {

static bool url_has_http_scheme(const std::string& url) {
  if (url.size() >= 7 && url.rfind("http://", 0) == 0) return true;
  if (url.size() >= 8 && url.rfind("https://", 0) == 0) return true;
  return false;
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

static std::string method_upper(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

Json::Value workflow_http_json_to_json(
  const DaemonConfig& cfg,
  const Json::Value& http_json,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  auto err_out = [&](const std::string& e) -> Json::Value {
    if (out_err) *out_err = e;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["assistant_text"] = "";
    o["error"] = e;
    return o;
  };

  if (!cfg.workflow_enable_http_tasks) {
    return err_out("http_json workflow tasks are disabled (start agentd with --workflow-enable-http-tasks)");
  }

  if (!http_json.isObject()) {
    return err_out("http_json must be an object");
  }

  const std::string url =
    http_json.isMember("url") && http_json["url"].isString() ? trim_copy(http_json["url"].asString()) : "";
  if (url.empty()) return err_out("http_json.url is required");
  if (!url_has_http_scheme(url)) return err_out("http_json.url must start with http:// or https://");
  if (url.size() > 4096) return err_out("http_json.url is too long");
  {
    std::string why;
    if (!workflow_http_url_is_allowed(cfg, url, &why)) {
      return err_out("http_json url is not allowed by workflow_http_allow_hosts: " + why);
    }
  }

  const std::string method_raw =
    http_json.isMember("method") && http_json["method"].isString() ? trim_copy(http_json["method"].asString()) : "POST";
  const std::string method = method_upper(method_raw);
  if (!(method == "GET" || method == "POST")) {
    return err_out("http_json.method must be GET or POST");
  }

  int64_t timeout_ms = 30000;
  if (http_json.isMember("timeout_ms") &&
      (http_json["timeout_ms"].isInt64() || http_json["timeout_ms"].isUInt64() || http_json["timeout_ms"].isInt() || http_json["timeout_ms"].isUInt())) {
    timeout_ms = http_json["timeout_ms"].asInt64();
  }
  if (timeout_ms < 1) timeout_ms = 1;
  if (timeout_ms > 300000) timeout_ms = 300000;

  size_t max_bytes = 1024 * 1024;
  if (http_json.isMember("max_bytes") && (http_json["max_bytes"].isInt64() || http_json["max_bytes"].isUInt64() || http_json["max_bytes"].isInt() || http_json["max_bytes"].isUInt())) {
    const int64_t v = http_json["max_bytes"].asInt64();
    if (v > 0) max_bytes = (size_t)v;
  }
  if (max_bytes < 1024) max_bytes = 1024;
  if (max_bytes > 16 * 1024 * 1024) max_bytes = 16 * 1024 * 1024;

  std::string body;
  if (http_json.isMember("body") && !http_json["body"].isNull()) {
    if (method == "GET") {
      return err_out("http_json.body is not allowed for method=GET");
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    body = Json::writeString(wb, http_json["body"]);
  }

  std::map<std::string, std::string> headers;
  // Defaults: make the response more likely to be JSON.
  headers["Accept"] = "application/json";
  if (!body.empty()) {
    headers["Content-Type"] = "application/json";
  }

  if (http_json.isMember("headers") && http_json["headers"].isObject()) {
    const auto& h = http_json["headers"];
    for (const auto& k : h.getMemberNames()) {
      if (!h[k].isString()) continue;
      headers[k] = h[k].asString();
    }
  }

  if (http_json.isMember("bearer_env") && http_json["bearer_env"].isString()) {
    const std::string env = trim_copy(http_json["bearer_env"].asString());
    if (!env.empty()) {
      if (!env_name_is_safe(env)) {
        return err_out("http_json.bearer_env must be a safe env var name");
      }
      const char* v = std::getenv(env.c_str());
      if (!v || !v[0]) {
        return err_out("http_json.bearer_env is set but the env var is missing/empty: " + env);
      }
      headers["Authorization"] = std::string("Bearer ") + v;
    }
  }

  const HttpClientResult r = http_request(url, method, headers, body, timeout_ms, max_bytes, cfg.proxy_url);

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["assistant_text"] = "";

  Json::Value http(Json::objectValue);
  http["status"] = (Json::Int64)r.http_status;
  http["response_text"] = r.response_body;
  if (r.retry_after_ms >= 0) http["retry_after_ms"] = (Json::Int64)r.retry_after_ms;
  out["http"] = http;

  if (!r.ok) {
    out["error"] = r.error.empty() ? "http request failed" : r.error;
    if (r.timed_out) {
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)250;
    } else if (r.response_too_large) {
      // Fail closed. Let the operator raise max_bytes explicitly if needed.
    } else {
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)250;
    }
    return out;
  }

  // Best-effort JSON parse for templating via json pointers.
  {
    Json::Value parsed;
    std::string perr;
    if (json_parse_any(r.response_body, &parsed, &perr)) {
      out["http"]["response_json"] = parsed;
    } else if (!r.response_body.empty()) {
      out["http"]["response_parse_error"] = perr;
    }
  }

  if (r.http_status >= 200 && r.http_status < 300) {
    out["ok"] = true;
    out["assistant_text"] = "http_json ok status=" + std::to_string((int)r.http_status);
    return out;
  }

  out["error"] = "http status " + std::to_string((int)r.http_status);

  // Retry surface for common transient statuses.
  const bool transient =
    (r.http_status == 408 || r.http_status == 429 || r.http_status == 500 || r.http_status == 502 || r.http_status == 503 || r.http_status == 504);
  if (transient) {
    out["retryable"] = true;
    int64_t retry_ms = 1000;
    if (r.retry_after_ms >= 0) retry_ms = r.retry_after_ms;
    if (retry_ms < 0) retry_ms = 0;
    if (retry_ms > 60 * 1000) retry_ms = 60 * 1000;
    out["retry_in_ms"] = (Json::Int64)retry_ms;
  }

  return out;
}

}  // namespace agentd
