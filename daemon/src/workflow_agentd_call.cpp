#include "workflow_agentd_call.h"

#include "http_client.h"
#include "http_allowlist.h"
#include "json_util.h"
#include "string_util.h"

#include "agent/json_c14n.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

namespace agentd {
namespace {

static bool json_parse_any_value(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = Json::Value(Json::nullValue);
  std::string perr;
  if (!json_parse_any(s, out, &perr)) {
    if (out_err) *out_err = perr;
    return false;
  }
  return true;
}

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static void sleep_ms(int ms) {
  if (ms <= 0) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

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

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static std::string sanitize_id_token(std::string s, size_t max_len) {
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
  while (!s.empty() && s.front() == '_') s.erase(s.begin());
  while (!s.empty() && s.back() == '_') s.pop_back();
  if (s.empty()) s = "msg";
  if (s.size() > max_len) s.resize(max_len);
  return s;
}

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path[0] == '/') return base + path;
  return base + "/" + path;
}

static bool is_terminal_workflow_status(const std::string& s) {
  return s == "done" || s == "error" || s == "cancelled";
}

static void add_final_hash_surface_best_effort(Json::Value* out) {
  if (!out || !out->isObject()) return;
  if (!out->isMember("agentd") || !(*out)["agentd"].isObject()) return;
  const Json::Value final = (*out)["agentd"].isMember("final") ? (*out)["agentd"]["final"] : Json::Value(Json::nullValue);
  if (!final.isObject()) return;
  const std::string final_text = json_stringify_compact(final);
  if (final_text.empty()) return;
  char token[80];
  char err[256];
  std::memset(token, 0, sizeof(token));
  std::memset(err, 0, sizeof(err));
  const agent_status_t st = agent_json_c14n_sha256_token(final_text.data(), final_text.size(), token, err, sizeof(err));
  if (st == AGENT_OK && token[0]) {
    (*out)["agentd"]["final_sha256"] = std::string(token);
    (*out)["agentd"]["final_sha256_alg"] = "agent_json_c14n_v1";
  } else if (err[0]) {
    (*out)["agentd"]["final_sha256_error"] = std::string(err);
  }
}

}  // namespace

Json::Value workflow_agentd_call_to_json(
  const DaemonConfig& cfg,
  const Json::Value& agentd_call,
  const std::string& task_trace_id,
  const std::string& previous_result_json,
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
    return err_out("agentd_call workflow tasks are disabled (start agentd with --workflow-enable-http-tasks)");
  }

  if (!agentd_call.isObject()) {
    return err_out("agentd_call must be an object");
  }

  const std::string base_url =
    agentd_call.isMember("base_url") && agentd_call["base_url"].isString()
    ? trim_copy(agentd_call["base_url"].asString())
    : "";
  if (base_url.empty()) return err_out("agentd_call.base_url is required");
  if (!url_has_http_scheme(base_url)) return err_out("agentd_call.base_url must start with http:// or https://");
  if (base_url.size() > 4096) return err_out("agentd_call.base_url is too long");
  WorkflowHttpUrlCheck base_check;
  {
    std::string why;
    if (!workflow_http_url_check(cfg, base_url, &base_check, &why)) {
      return err_out("agentd_call base_url is not allowed by workflow_http outbound policy: " + why);
    }
    if (cfg.workflow_http_dns_pin && !base_check.host_is_ip && base_check.resolved_addrs.empty()) {
      return err_out("agentd_call DNS pin is enabled but DNS resolution produced no addresses for base_url host");
    }
  }

  const std::string op =
    agentd_call.isMember("op") && agentd_call["op"].isString() ? trim_copy(agentd_call["op"].asString()) : "workflow_submit_and_wait";
  if (op != "workflow_submit_and_wait") {
    return err_out("agentd_call.op must be workflow_submit_and_wait");
  }

  int64_t timeout_ms = 30000;
  if (agentd_call.isMember("timeout_ms") &&
      (agentd_call["timeout_ms"].isInt64() || agentd_call["timeout_ms"].isUInt64() || agentd_call["timeout_ms"].isInt() || agentd_call["timeout_ms"].isUInt())) {
    timeout_ms = agentd_call["timeout_ms"].asInt64();
  }
  if (timeout_ms < 1) timeout_ms = 1;
  if (timeout_ms > 300000) timeout_ms = 300000;

  int64_t poll_ms = 50;
  if (agentd_call.isMember("poll_ms") && (agentd_call["poll_ms"].isInt64() || agentd_call["poll_ms"].isUInt64() || agentd_call["poll_ms"].isInt() || agentd_call["poll_ms"].isUInt())) {
    poll_ms = agentd_call["poll_ms"].asInt64();
  }
  if (poll_ms < 10) poll_ms = 10;
  if (poll_ms > 1000) poll_ms = 1000;

  size_t max_bytes = 1024 * 1024;
  if (agentd_call.isMember("max_bytes") &&
      (agentd_call["max_bytes"].isInt64() || agentd_call["max_bytes"].isUInt64() || agentd_call["max_bytes"].isInt() || agentd_call["max_bytes"].isUInt())) {
    const int64_t v = agentd_call["max_bytes"].asInt64();
    if (v > 0) max_bytes = (size_t)v;
  }
  if (max_bytes < 1024) max_bytes = 1024;
  if (max_bytes > 16 * 1024 * 1024) max_bytes = 16 * 1024 * 1024;

  const bool include_tasks =
    agentd_call.isMember("include_tasks") && agentd_call["include_tasks"].isBool()
    ? agentd_call["include_tasks"].asBool()
    : false;
  const bool include_results =
    !agentd_call.isMember("include_results") || (agentd_call["include_results"].isBool() && agentd_call["include_results"].asBool());

  std::map<std::string, std::string> headers;
  headers["Accept"] = "application/json";
  headers["Content-Type"] = "application/json";

  if (agentd_call.isMember("headers") && agentd_call["headers"].isObject()) {
    const auto& h = agentd_call["headers"];
    for (const auto& k : h.getMemberNames()) {
      if (!h[k].isString()) continue;
      const std::string lk = lower_copy(k);
      if (lk == "authorization") {
        return err_out("agentd_call.headers.Authorization is not allowed (use bearer_env)");
      }
      headers[k] = h[k].asString();
    }
  }

  if (agentd_call.isMember("bearer_env") && agentd_call["bearer_env"].isString()) {
    const std::string env = trim_copy(agentd_call["bearer_env"].asString());
    if (!env.empty()) {
      if (!env_name_is_safe(env)) {
        return err_out("agentd_call.bearer_env must be a safe env var name");
      }
      const char* v = std::getenv(env.c_str());
      if (!v || !v[0]) {
        return err_out("agentd_call.bearer_env is set but the env var is missing/empty: " + env);
      }
      headers["Authorization"] = std::string("Bearer ") + v;
    }
  }

  // Best-effort: reuse remote workflow_id from a previous attempt result to avoid duplicate submit.
  std::string remote_workflow_id;
  if (!previous_result_json.empty()) {
    Json::Value prev;
    std::string perr;
    if (json_parse_any_value(previous_result_json, &prev, &perr) && prev.isObject()) {
      const std::string prev_base =
        prev.isMember("agentd") && prev["agentd"].isObject() && prev["agentd"].isMember("base_url") && prev["agentd"]["base_url"].isString()
        ? trim_copy(prev["agentd"]["base_url"].asString())
        : "";
      const std::string prev_wid =
        prev.isMember("agentd") && prev["agentd"].isObject() && prev["agentd"].isMember("workflow_id") && prev["agentd"]["workflow_id"].isString()
        ? trim_copy(prev["agentd"]["workflow_id"].asString())
        : "";
      if (!prev_wid.empty() && (prev_base.empty() || prev_base == base_url)) {
        remote_workflow_id = prev_wid;
      }

      // If the previous attempt already captured a terminal remote workflow response, reuse it without doing any network I/O.
      // This avoids repeated charges/retries when the local workflow engine reclaims the task after a restart.
      if (prev.isMember("agentd") && prev["agentd"].isObject() && prev["agentd"].isMember("final") && prev["agentd"]["final"].isObject()) {
        const Json::Value final = prev["agentd"]["final"];
        const std::string st =
          final.isMember("workflow") && final["workflow"].isObject() && final["workflow"].isMember("status") && final["workflow"]["status"].isString()
          ? trim_copy(final["workflow"]["status"].asString())
          : "";
        if (!st.empty() && is_terminal_workflow_status(st)) {
          Json::Value out = prev;
          out["kind"] = "agentd_call";
          // Ensure base_url/op are present/canonical for debugging.
          if (!out.isMember("agentd") || !out["agentd"].isObject()) out["agentd"] = Json::Value(Json::objectValue);
          out["agentd"]["base_url"] = base_url;
          out["agentd"]["op"] = op;
          // Avoid budget double-charging on replay-only attempts.
          out["tool_calls_total"] = (Json::Int64)0;
          out["steps_executed"] = (Json::Int64)0;
          if (st == "done") {
            out["ok"] = true;
            out["assistant_text"] = "agentd_call: remote workflow done (replayed)";
            if (out.isMember("error")) out.removeMember("error");
          } else {
            out["ok"] = false;
            if (!out.isMember("error")) out["error"] = "remote workflow status " + st;
            out["assistant_text"] = "";
          }
          add_final_hash_surface_best_effort(&out);
          return out;
        }
      }
    }
  }

  Json::Value out(Json::objectValue);
  out["kind"] = "agentd_call";
  out["ok"] = false;
  out["assistant_text"] = "";
  out["agentd"] = Json::Value(Json::objectValue);
  out["agentd"]["base_url"] = base_url;
  out["agentd"]["op"] = op;

  const std::string submit_url = join_base_path(base_url, "/api/v1/workflow/submit");
  HttpClientPinnedResolve pin;
  const HttpClientPinnedResolve* pinp = nullptr;
  if (cfg.workflow_http_dns_pin && !base_check.host_is_ip) {
    pin.host = base_check.host;
    pin.port = base_check.port;
    // Prefer IPv4 pinning when both families are available.
    for (const auto& ip : base_check.resolved_addrs) {
      if (!ip.empty() && ip.find(':') == std::string::npos) pin.addrs.push_back(ip);
    }
    if (pin.addrs.empty()) pin.addrs = base_check.resolved_addrs;
    pinp = &pin;
  }

  if (remote_workflow_id.empty()) {
    const Json::Value wf = agentd_call.isMember("workflow") ? agentd_call["workflow"] : Json::Value(Json::nullValue);
    if (!wf.isObject()) return err_out("agentd_call.workflow is required and must be an object");

    Json::Value wf2 = wf;
    if (!wf2.isMember("trace_id") || !wf2["trace_id"].isString() || trim_copy(wf2["trace_id"].asString()).empty()) {
      const std::string t = !task_trace_id.empty() ? task_trace_id : "agentd_call";
      wf2["trace_id"] = sanitize_id_token(t, 128);
    }
    if (!wf2.isMember("idempotency_key") || !wf2["idempotency_key"].isString() || trim_copy(wf2["idempotency_key"].asString()).empty()) {
      const std::string t = !task_trace_id.empty() ? task_trace_id : "agentd_call";
      wf2["idempotency_key"] = sanitize_id_token(std::string("agentd_call:") + t, 128);
    }

    const std::string body = json_stringify_compact(wf2);
    const HttpClientResult r = http_request(submit_url, "POST", headers, body, timeout_ms, max_bytes, cfg.proxy_url, pinp);

    Json::Value submit_http(Json::objectValue);
    submit_http["status"] = (Json::Int64)r.http_status;
    submit_http["response_text"] = r.response_body;
    if (r.retry_after_ms >= 0) submit_http["retry_after_ms"] = (Json::Int64)r.retry_after_ms;
    if (!r.resolved_addrs.empty()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& ip : r.resolved_addrs) {
        if (!ip.empty()) arr.append(ip);
      }
      if (!arr.empty()) submit_http["resolved_addrs"] = arr;
    }
    out["agentd"]["submit_http"] = submit_http;

    if (!r.ok) {
      out["error"] = r.error.empty() ? "agentd_call submit http request failed" : r.error;
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)poll_ms;
      return out;
    }

    Json::Value submit_json;
    std::string jerr;
    if (json_parse_any_value(r.response_body, &submit_json, &jerr) && submit_json.isObject()) {
      out["agentd"]["submit_json"] = submit_json;
      if (submit_json.isMember("workflow_id") && submit_json["workflow_id"].isString()) {
        remote_workflow_id = trim_copy(submit_json["workflow_id"].asString());
      }
      const bool ok = submit_json.isMember("ok") && submit_json["ok"].isBool() && submit_json["ok"].asBool();
      if (!ok && submit_json.isMember("error") && submit_json["error"].isString()) {
        out["error"] = submit_json["error"].asString();
      }
    } else if (!r.response_body.empty()) {
      out["agentd"]["submit_parse_error"] = jerr;
    }

    if (r.http_status < 200 || r.http_status >= 300) {
      if (!out.isMember("error")) out["error"] = "agentd_call submit http status " + std::to_string((int)r.http_status);
      const bool transient =
        (r.http_status == 408 || r.http_status == 429 || r.http_status == 500 || r.http_status == 502 || r.http_status == 503 || r.http_status == 504);
      if (transient) {
        out["retryable"] = true;
        int64_t retry_ms = 1000;
        if (r.retry_after_ms >= 0) retry_ms = r.retry_after_ms;
        retry_ms = std::max<int64_t>(0, std::min<int64_t>(60000, retry_ms));
        out["retry_in_ms"] = (Json::Int64)retry_ms;
      }
      return out;
    }

    if (remote_workflow_id.empty()) {
      out["error"] = "agentd_call submit succeeded but response missing workflow_id";
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)poll_ms;
      return out;
    }
  }

  out["agentd"]["workflow_id"] = remote_workflow_id;

  // Poll until terminal or attempt-level timeout.
  const int64_t deadline_ms = unix_ms_now() + timeout_ms;
  std::string final_url = join_base_path(base_url, "/api/v1/workflow");
  final_url += "?workflow_id=" + remote_workflow_id;
  if (include_tasks) final_url += "&include_tasks=1";
  if (include_results) final_url += "&include_results=1";

  while (unix_ms_now() <= deadline_ms) {
    const HttpClientResult r = http_request(final_url, "GET", headers, /*body=*/"", timeout_ms, max_bytes, cfg.proxy_url, pinp);

    Json::Value poll_http(Json::objectValue);
    poll_http["status"] = (Json::Int64)r.http_status;
    poll_http["response_text"] = r.response_body;
    if (r.retry_after_ms >= 0) poll_http["retry_after_ms"] = (Json::Int64)r.retry_after_ms;
    if (!r.resolved_addrs.empty()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& ip : r.resolved_addrs) {
        if (!ip.empty()) arr.append(ip);
      }
      if (!arr.empty()) poll_http["resolved_addrs"] = arr;
    }
    out["agentd"]["poll_http"] = poll_http;

    if (!r.ok) {
      out["error"] = r.error.empty() ? "agentd_call poll http request failed" : r.error;
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)poll_ms;
      return out;
    }

    Json::Value final;
    std::string perr;
    if (!json_parse_any_value(r.response_body, &final, &perr) || !final.isObject()) {
      out["error"] = "agentd_call poll: failed to parse remote JSON response";
      if (!perr.empty()) out["parse_error"] = perr;
      out["retryable"] = true;
      out["retry_in_ms"] = (Json::Int64)poll_ms;
      return out;
    }
    out["agentd"]["final"] = final;

    const bool ok_flag = final.isMember("ok") && final["ok"].isBool() && final["ok"].asBool();
    if (!ok_flag && final.isMember("error") && final["error"].isString()) {
      out["error"] = final["error"].asString();
    }

    const std::string st =
      final.isMember("workflow") && final["workflow"].isObject() && final["workflow"].isMember("status") && final["workflow"]["status"].isString()
      ? trim_copy(final["workflow"]["status"].asString())
      : "";
    if (!st.empty() && is_terminal_workflow_status(st)) {
      if (st == "done") {
        out["ok"] = true;
        out["assistant_text"] = "agentd_call: remote workflow done";
      } else {
        out["ok"] = false;
        if (!out.isMember("error")) out["error"] = "remote workflow status " + st;
        out["assistant_text"] = "";
      }
      add_final_hash_surface_best_effort(&out);
      return out;
    }

    // Not terminal yet. Sleep in a simple loop (no busy spin).
    // Note: using delay_ms in the workflow engine would be overkill; this is a deterministic task attempt.
    sleep_ms((int)poll_ms);
  }

  out["error"] = "agentd_call timeout waiting for remote workflow";
  out["retryable"] = true;
  out["retry_in_ms"] = (Json::Int64)poll_ms;
  return out;
}

}  // namespace agentd
