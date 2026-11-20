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
#include <functional>
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

  // Preferred stable surface for quorum/correlation: hash a **stable projection** of the remote `final.result`.
  // Remote results include telemetry and timestamps (e.g. elapsed_ms) which are expected to vary across agents/runs.
  // We prune known-ephemeral keys and then hash the remaining JSON (agent_json_c14n_v1).
  if (final.isMember("result")) {
    Json::Value stable = final["result"];
    std::function<void(Json::Value*)> prune = [&](Json::Value* v) {
      if (!v) return;
      if (v->isObject()) {
        // Drop known-ephemeral keys (timestamps, ids, counters).
        // This list is intentionally conservative: remove telemetry, keep semantic result content.
        static const char* kDropKeys[] = {
          "attempt",
          "created_unix_ms",
          "updated_unix_ms",
          "started_unix_ms",
          "finished_unix_ms",
          "ready_unix_ms",
          "elapsed_ms",
          "elapsed_ms_cum",
          // Derived hash surfaces should not influence semantic-result hashing.
          "final_sha256",
          "final_sha256_alg",
          "result_sha256",
          "result_sha256_alg",
          "result_sha256_schema",
          "result_sha256_error",
          "prompt_tokens",
          "completion_tokens",
          "total_tokens",
          "tool_calls_total",
          "tool_calls_total_cum",
          "steps_executed",
          "steps_executed_cum",
          "retryable",
          "retry_in_ms",
          "trace_id",
          "workflow_id",
          "tasks"
        };
        for (const char* k : kDropKeys) {
          if (v->isMember(k)) v->removeMember(k);
        }
        for (const auto& k : v->getMemberNames()) {
          Json::Value& child = (*v)[k];
          prune(&child);
        }
      } else if (v->isArray()) {
        for (Json::ArrayIndex i = 0; i < v->size(); i++) {
          Json::Value& child = (*v)[i];
          prune(&child);
        }
      }
    };
    prune(&stable);

    const std::string stable_text = json_stringify_compact(stable);
    if (!stable_text.empty()) {
      char token[80];
      char err[256];
      std::memset(token, 0, sizeof(token));
      std::memset(err, 0, sizeof(err));
      const agent_status_t st = agent_json_c14n_sha256_token(stable_text.data(), stable_text.size(), token, err, sizeof(err));
      if (st == AGENT_OK && token[0]) {
        (*out)["agentd"]["result_sha256"] = std::string(token);
        (*out)["agentd"]["result_sha256_alg"] = "agent_json_c14n_v1";
        (*out)["agentd"]["result_sha256_schema"] = "agentd_call_result_stable_v1";
      } else if (err[0]) {
        (*out)["agentd"]["result_sha256_error"] = std::string(err);
      }
    }
  }

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

namespace {
constexpr size_t kRemoteReceiptMax = 8 * 1024 * 1024;
std::string remote_hash(const Json::Value& value) {
  const auto raw = json_stringify_compact(value);
  if (raw.size() > kRemoteReceiptMax) return {};
  char digest[80]{}, error[160]{};
  if (agent_json_c14n_sha256_token(raw.data(), raw.size(), digest, error, sizeof(error)) != AGENT_OK) return {};
  return digest;
}
Json::Value remote_failure(const std::string& reason) {
  Json::Value out; out["ok"] = false; out["error"] = reason; out["assistant_text"] = "";
  out["retryable"] = false; out["tool_calls_total"] = Json::Int64(0); out["steps_executed"] = Json::Int64(0);
  return out;
}
}
WorkflowAgentdCallEffect::WorkflowAgentdCallEffect(WorkflowEffectContext context, const Json::Value& call, bool last_attempt, bool previously_attempted)
    : context_(std::move(context)), call_(call), last_attempt_(last_attempt), previously_attempted_(previously_attempted) {
  definition_.workflow_id = context_.workflow_id; definition_.task_id = context_.task_id; definition_.mode = "guarded";
  Json::Value identity(Json::arrayValue); identity.append(context_.workflow_id); identity.append(context_.task_id);
  const auto digest = remote_hash(identity);
  if (!digest.empty()) definition_.effect_id = "afx_" + digest.substr(7);
  Json::Value request; request["protocol"] = "agentd.remote_workflow.v1"; request["call"] = call;
  definition_.request_hash = remote_hash(request);
  if (!call.isObject() || !call["workflow"].isObject() || definition_.effect_id.empty() || definition_.request_hash.empty())
    identity_error_ = "remote workflow request cannot be durably identified";
}
Json::Value WorkflowAgentdCallEffect::submission() const {
  auto result = call_["workflow"];
  result["idempotency_key"] = remote_key_;
  result["trace_id"] = remote_key_;
  return result;
}
Json::Value WorkflowAgentdCallEffect::admission() const {
  Json::Value value; value["schema"] = "agentd.remote_workflow.admission.v1";
  value["effect_id"] = definition_.effect_id; value["request_hash"] = definition_.request_hash;
  value["submission_hash"] = remote_hash(submission());value["remote_key"] = remote_key_;
  return value;
}
void WorkflowAgentdCallEffect::annotate(Json::Value* result, bool replayed) const {
  if (row_.effect_id.empty()) return;
  auto& effect = (*result)["effect"];
  effect["effect_id"] = row_.effect_id; effect["request_hash"] = row_.request_hash;
  effect["mode"] = row_.mode; effect["state"] = row_.state;
  effect["revision"] = Json::Int64(row_.revision); effect["attempt"] = Json::Int64(row_.attempt);
  effect["boundary"] = "agentd_call"; effect["replayed"] = replayed;
}
Json::Value WorkflowAgentdCallEffect::pending(const std::string& reason, bool retryable) const {
  auto out = remote_failure(reason); annotate(&out);
  out["kind"] = "agentd_call"; out["retryable"] = retryable;
  if (retryable) out["retry_in_ms"] = Json::Int64(250);
  if (retryable && !last_attempt_) return out;
  out["needs_input"] = true;
  auto& blocker = out["blocker"];
  blocker["id"] = definition_.effect_id; blocker["kind"] = "remote_workflow_outcome_unknown";
  blocker["reason"] = reason; blocker["workflow_id"] = context_.workflow_id; blocker["task_id"] = context_.task_id;
  blocker["required_action"] = row_.effect_id.empty() ?
    "Inspect the earlier remote submission and its original receipt/trace. No durable admission is available; do not recreate it by submitting another workflow." :
    "Check the original remote workflow using its recorded target and key " + remote_key_ +
    ". Restore remote access or its original durable receipt; do not submit a replacement. Reconciliation performs reads only.";
  blocker["attempts"] = Json::Value(Json::arrayValue);
  blocker["attempts"].append("Inspected the immutable local admission; another execution POST is forbidden.");
  blocker["attempts"].append(reason);
  return out;
}
bool WorkflowAgentdCallEffect::matches_workflow(const Json::Value& response, std::string* workflow_id) const {
  if (!response.isObject() || !call_.isObject() || !call_["workflow"].isObject()) return false;
  const auto& workflow = response["workflow"];
  if (response["ok"] != true || !workflow.isObject() ||
      !workflow["workflow_id"].isString() || workflow["workflow_id"].asString().empty() ||
      workflow["workflow_id"].asString().size() > 128 || workflow["idempotency_key"] != remote_key_ ||
      workflow["trace_id"] != remote_key_) return false;
  const auto& wanted = call_["workflow"]["session_id"];
  const auto scope = call_["workflow"]["allow_sessions"] == true && wanted.isString() ? trim_copy(wanted.asString()) : "";
  const auto& actual = workflow["session_id"];
  if (!actual.isNull() && !actual.isString()) return false;
  if (scope != (actual.isString() ? actual.asString() : "")) return false;
  if (!workflow["status"].isString()) return false;
  const auto state = workflow["status"].asString();
  if (state != "queued" && state != "running" && !is_terminal_workflow_status(state)) return false;
  if (!remote_workflow_id_.empty() && workflow["workflow_id"] != remote_workflow_id_) return false;
  if (workflow_id) *workflow_id = workflow["workflow_id"].asString();
  return true;
}
Json::Value WorkflowAgentdCallEffect::preflight() {
  checked_ = false;
  if (context_.should_cancel && context_.should_cancel()) { auto out = remote_failure("cancelled");out["cancelled"] = true;return out; }
  if (!identity_error_.empty()) return remote_failure(identity_error_);
  if (!context_.db || !context_.db->is_open() || context_.workflow_id.empty() || context_.task_id.empty())
    return remote_failure("durable remote-workflow context unavailable; submission refused");
  bool found = false; std::string error, space;
  if (!context_.db->workflow_remote_namespace(&space, &error)) return remote_failure(error);
  Json::Value remote_identity(Json::arrayValue);remote_identity.append(space);remote_identity.append(definition_.effect_id);
  const auto digest = remote_hash(remote_identity);
  if (digest.empty()) return remote_failure("remote namespace cannot be hashed");
  remote_key_ = "remote_" + digest.substr(7);
  if (!context_.db->get_workflow_effect(definition_.effect_id, &row_, &found, &error)) return remote_failure(error);
  if (!found) {
    if (previously_attempted_) return pending("earlier remote attempt has no durable admission; fresh deployment or original-outcome review is required", false);
    checked_ = true;return Json::Value();
  }
  if (row_.workflow_id != definition_.workflow_id || row_.task_id != definition_.task_id ||
      row_.request_hash != definition_.request_hash || row_.mode != "guarded" || !row_.effect_slot.empty())
    return pending("remote workflow identity or request changed; no network request was made", false);
  if (row_.state == "prepared") { checked_ = true;return Json::Value(); }
  Json::Value receipt;
  if (row_.receipt_json.size() > kRemoteReceiptMax || !json_parse_any_value(row_.receipt_json, &receipt, &error) || !receipt.isObject())
    return pending("remote workflow admission/receipt is invalid; no network request was made", false);
  if (row_.state == "dispatching") {
    if (receipt.isMember("remote_workflow_id")) {
      if (!receipt["remote_workflow_id"].isString() || receipt["remote_workflow_id"].asString().empty() ||
          receipt["remote_workflow_id"].asString().size() > 128) return pending("invalid saved remote workflow identity", false);
      remote_workflow_id_ = receipt["remote_workflow_id"].asString();receipt.removeMember("remote_workflow_id");
    }
    if (receipt == admission()) { checked_ = true;return Json::Value(); }
  }
  if (row_.state != "completed" || receipt["schema"] != "agentd.remote_workflow.receipt.v1" ||
      receipt["effect_id"] != definition_.effect_id || receipt["request_hash"] != definition_.request_hash ||
      !receipt["result"].isObject() || !receipt["result"]["ok"].isBool())
    return pending("remote workflow receipt cannot establish a terminal outcome", false);
  const auto checksum = receipt["checksum"].isString() ? receipt["checksum"].asString() : "";
  receipt.removeMember("checksum");
  if (!receipt["result"]["agentd"].isObject()) return pending("remote receipt has no original workflow", false);
  const auto& final = receipt["result"]["agentd"]["final"];
  if (checksum.empty() || checksum != remote_hash(receipt) || !matches_workflow(final) ||
      !is_terminal_workflow_status(final["workflow"]["status"].asString()))
    return pending("remote workflow terminal receipt is inconsistent; no network request was made", false);
  auto out = receipt["result"]; annotate(&out, true);out["retryable"] = false;
  for (const char* key : {"tool_calls_total", "steps_executed", "elapsed_ms", "total_tokens", "token_budget_charge"}) {
    if (out.isMember(key)) out["effect"]["original_usage"][key] = out[key];
    out[key] = Json::Int64(0);
  }
  return out;
}
bool WorkflowAgentdCallEffect::admit(std::string* error) {
  if (!checked_ || !needs_submission()) { if (error) *error = "remote submission was already admitted";return false; }
  if (context_.should_cancel && context_.should_cancel()) { if (error) *error = "cancelled";return false; }
  if (!context_.db->prepare_workflow_effect(definition_, &row_, error) || row_.state != "prepared") return false;
  AgentDb::WorkflowEffectRow next;
  if (!context_.db->transition_workflow_effect(row_, "dispatching", json_stringify_compact(admission()), &next, error)) return false;
  row_ = std::move(next);return true;
}
bool WorkflowAgentdCallEffect::observe_workflow(const Json::Value& response, std::string* error) {
  std::string id;
  if (!checked_ || row_.state != "dispatching" || !matches_workflow(response, &id)) return false;
  if (!remote_workflow_id_.empty()) return remote_workflow_id_ == id;
  auto value = admission();value["remote_workflow_id"] = id;
  AgentDb::WorkflowEffectRow saved;
  if (!context_.db->checkpoint_workflow_effect(row_, json_stringify_compact(value), false, &saved, error)) return false;
  row_ = std::move(saved);remote_workflow_id_ = id;return true;
}
Json::Value WorkflowAgentdCallEffect::complete(Json::Value result) {
  if (!checked_ || row_.state != "dispatching" || !result.isObject() || !result["ok"].isBool() || !result["agentd"].isObject() ||
      !matches_workflow(result["agentd"]["final"]) ||
      !is_terminal_workflow_status(result["agentd"]["final"]["workflow"]["status"].asString()))
    return pending("remote response is not a matching terminal receipt", false);
  Json::Value receipt; receipt["schema"] = "agentd.remote_workflow.receipt.v1";
  receipt["effect_id"] = definition_.effect_id; receipt["request_hash"] = definition_.request_hash;
  result["retryable"] = false;receipt["result"] = result;
  const auto checksum = remote_hash(receipt);
  if (checksum.empty()) return pending("remote terminal receipt exceeds its 8-MiB bound", false);
  receipt["checksum"] = checksum;
  const auto raw = json_stringify_compact(receipt);
  if (raw.size() > kRemoteReceiptMax) return pending("remote terminal receipt exceeds its 8-MiB bound", false);
  AgentDb::WorkflowEffectRow saved; std::string error;
  if (!context_.db->transition_workflow_effect(row_, "completed", raw, &saved, &error))
    return pending("remote terminal receipt was not saved: " + error, false);
  row_ = std::move(saved);annotate(&result);return result;
}

Json::Value workflow_agentd_call_to_json(
  const DaemonConfig& cfg,
  const Json::Value& agentd_call,
  const std::string& task_trace_id,
  WorkflowAgentdCallEffect& effect,
  const WorkflowEffectContext& context,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  auto err_out = [&](const std::string& e) -> Json::Value {
    if (out_err) *out_err = e;
    return effect.needs_submission() ? remote_failure(e) : effect.pending(e, false);
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

  const std::string target_id =
    agentd_call.isMember("target_id") && agentd_call["target_id"].isString()
    ? trim_copy(agentd_call["target_id"].asString())
    : "";
  const std::string target_identity =
    agentd_call.isMember("target_identity") && agentd_call["target_identity"].isString()
    ? trim_copy(agentd_call["target_identity"].asString())
    : std::string("url:") + base_url;
  const std::string broker_agent_id =
    agentd_call.isMember("broker_agent_id") && agentd_call["broker_agent_id"].isString()
    ? trim_copy(agentd_call["broker_agent_id"].asString())
    : "";
  const std::string broker_deployment_id =
    agentd_call.isMember("broker_deployment_id") && agentd_call["broker_deployment_id"].isString()
    ? trim_copy(agentd_call["broker_deployment_id"].asString())
    : "";

  auto attach_target_metadata = [&]() -> Json::Value {
    Json::Value meta(Json::objectValue);
    meta["base_url"] = base_url;
    meta["op"] = op;
    if (!target_id.empty()) meta["target_id"] = target_id;
    if (!target_identity.empty()) meta["target_identity"] = target_identity;
    if (!broker_agent_id.empty()) meta["broker_agent_id"] = broker_agent_id;
    if (!broker_deployment_id.empty()) meta["broker_deployment_id"] = broker_deployment_id;
    return meta;
  };

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

  HttpClientPinnedResolve pin;
  const HttpClientPinnedResolve* pinp = nullptr;
  if (cfg.workflow_http_dns_pin && !base_check.host_is_ip) {
    pin.host = base_check.host;pin.port = base_check.port;
    for (const auto& ip : base_check.resolved_addrs) if (!ip.empty() && ip.find(':') == std::string::npos) pin.addrs.push_back(ip);
    if (pin.addrs.empty()) pin.addrs = base_check.resolved_addrs;
    pinp = &pin;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int64_t calls = 0;
  const auto cancelled = [&]() { return context.should_cancel && context.should_cancel(); };
  const auto remaining = [&]() { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count(); };
  const auto network_available = [&]() {
    return !cancelled() && remaining() > 0 && (context.max_network_calls <= 0 || calls < context.max_network_calls);
  };
  auto decorate = [&](Json::Value result) {
    result["kind"] = "agentd_call";result["tool_calls_total"] = Json::Int64(calls);result["steps_executed"] = Json::Int64(calls);
    if (!result["agentd"].isObject()) result["agentd"] = attach_target_metadata();
    result["agentd"]["idempotency_key"] = effect.remote_key();
    if (!task_trace_id.empty()) result["agentd"]["parent_trace_id"] = task_trace_id;
    if (cancelled()) { result["cancelled"] = true;result["retryable"] = false; }
    return result;
  };
  auto pending = [&](const std::string& reason, bool retryable) { return decorate(effect.pending(reason, retryable)); };
  auto request = [&](const std::string& url, const std::string& method, const std::string& body) {
    ++calls;
    auto callback = [](void* ptr) { return (*static_cast<const std::function<bool()>*>(ptr))(); };
    const std::function<bool()> cancel = cancelled;
    return http_request(url, method, headers, body, std::max<int64_t>(1, remaining()), max_bytes, cfg.proxy_url, pinp,
                        callback, const_cast<std::function<bool()>*>(&cancel), false);
  };
  if (effect.needs_submission()) {
    if (!network_available()) return decorate(remote_failure("no remote submission grant or deadline remains"));
    std::string error;
    if (!effect.admit(&error)) return pending("remote submission admission failed: " + error, false);
    // The immutable admission precedes HTTP. Every later attempt, including
    // timeout/HTTP rejection/malformed acceptance, can only look up this key.
    const auto submitted = request(join_base_path(base_url, "/api/v1/workflow/submit"), "POST", json_stringify_compact(effect.submission()));
    (void)submitted;
    // Rejections and malformed/lost replies are not nonexecution proof. Use the
    // authenticated lookup below for every outcome; never retry this POST.
  }
  const auto encode_query = [](const std::string& input) {
    std::string output;const char* hex = "0123456789ABCDEF";
    for (const unsigned char c : input) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') output += char(c);
      else { output += '%';output += hex[c >> 4];output += hex[c & 15]; }
    }
    return output;
  };
  const auto submission = effect.submission();
  const auto session = submission["allow_sessions"] == true && submission["session_id"].isString() ? trim_copy(submission["session_id"].asString()) : "";
  std::string url = join_base_path(base_url, "/api/v1/workflow") + "?idempotency_key=" + effect.remote_key() +
    "&session_id=" + encode_query(session) + "&include_tasks=" + (include_tasks ? "1" : "0") +
    "&include_results=" + (include_results ? "1" : "0");
  int unavailable_reads = 0;
  const auto pause = [&](int64_t milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!cancelled() && std::chrono::steady_clock::now() < until && remaining() > 0) sleep_ms(10);
  };
  while (network_available()) {
    const auto response = request(url, "GET", "");
    if (!response.ok || response.http_status < 200 || response.http_status >= 300) {
      const bool transient = response.http_status == 0 || response.http_status == 404 || response.http_status == 408 ||
        response.http_status == 429 || response.http_status == 500 || response.http_status == 502 || response.http_status == 503 || response.http_status == 504;
      if (transient && !response.response_too_large && ++unavailable_reads < 3 && network_available()) {
        pause(unavailable_reads == 1 ? 250 : 1000);continue;
      }
      return pending("original remote workflow lookup unavailable (HTTP " + std::to_string(response.http_status) + "); no execution was repeated",
                     transient && !response.response_too_large);
    }
    Json::Value final;std::string error, remote_id;
    if (!json_parse_any_value(response.response_body, &final, &error) || !effect.matches_workflow(final, &remote_id))
      return pending("remote workflow lookup returned invalid or mismatching identity/state; no execution was repeated", false);
    unavailable_reads = 0;
    if (!effect.observe_workflow(final, &error)) return pending("original remote workflow identity was not saved: " + error, false);
    const auto status = final["workflow"]["status"].asString();
    if (is_terminal_workflow_status(status)) {
      Json::Value result;result["ok"] = status == "done";
      result["assistant_text"] = status == "done" ? "agentd_call: remote workflow done" : "";
      if (status != "done") result["error"] = "remote workflow status " + status;
      result["agentd"] = attach_target_metadata();result["agentd"]["workflow_id"] = remote_id;result["agentd"]["final"] = final;
      add_final_hash_surface_best_effort(&result);
      return effect.complete(decorate(std::move(result)));
    }
    pause(poll_ms);
  }
  return pending("remote workflow wait exhausted its attempt deadline or network grant; only original-workflow reads may continue", true);

}

}  // namespace agentd
