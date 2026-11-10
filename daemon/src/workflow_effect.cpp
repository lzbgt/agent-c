#include "workflow_effect.h"
#include "agent/json_c14n.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <unordered_set>

namespace agentd {
namespace {
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer,value);
}
std::string hash(const Json::Value& value) {
  const auto input = encode(value);
  char digest[80] = {}, error[160] = {};
  if (agent_json_c14n_sha256_token(input.data(),input.size(),digest,error,sizeof(error)) != AGENT_OK) return {};
  return digest;
}
std::string origin(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  const auto end = url.find_first_of("/?#",scheme+3);
  const auto authority = url.substr(0,end);
  if (authority.find('@') != std::string::npos) return {};
  return lower_copy(authority);
}
Json::Value failure(const std::string& error, bool retry = false) {
  Json::Value out(Json::objectValue);
  out["ok"] = false; out["assistant_text"] = ""; out["error"] = error;
  out["retryable"] = retry;
  if (retry) out["retry_in_ms"] = Json::Int64(1000);
  return out;
}
void metadata(Json::Value* out, const AgentDb::WorkflowEffectRow& row, bool replayed = false) {
  auto& effect = (*out)["effect"];
  effect["effect_id"] = row.effect_id; effect["request_hash"] = row.request_hash;
  effect["mode"] = row.mode; effect["state"] = row.state;
  effect["revision"] = Json::Int64(row.revision); effect["attempt"] = Json::Int64(row.attempt);
  effect["replayed"] = replayed;
}
Json::Value unresolved(const AgentDb::WorkflowEffectRow& row, const std::string& reason, bool retry) {
  auto out = failure(reason,retry);
  metadata(&out,row);
  if (!retry) {
    out["needs_input"] = true;
    auto& blocker = out["blocker"];
    blocker["id"] = row.effect_id;
    blocker["kind"] = "external_effect_unknown";
    blocker["reason"] = reason;
    blocker["required_action"] = "Verify the destination receipt for " + row.effect_id +
      " and provide a matching terminal reconciliation response. Do not repeat the write without proof that the prior attempt cannot commit.";
    blocker["workflow_id"] = row.workflow_id; blocker["task_id"] = row.task_id;
    blocker["attempts"] = Json::Value(Json::arrayValue);
    blocker["attempts"].append("Inspected the durable dispatch/receipt record; a successful outcome is not established.");
    blocker["attempts"].append(reason);
  }
  return out;
}
}

bool workflow_effect_validate_http(const Json::Value& spec, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  if (!spec.isObject()) return fail("http_json must be an object");
  if (!spec["url"].isString() || (spec.isMember("method") && !spec["method"].isString())) return fail("http_json URL/method must be strings");
  const auto method = lower_copy(trim_copy(spec.get("method","POST").asString()));
  if (method == "get" && spec.isMember("effect")) return fail("http_json.effect is only valid for POST");
  if (spec.isMember("effect")) {
    const auto& effect = spec["effect"];
    if (!effect.isObject()) return fail("http_json.effect must be an object");
    for (const auto& field : effect.getMemberNames()) if (field != "mode" && field != "reconcile_url") return fail("unknown http_json.effect field: " + field);
    if (effect.isMember("mode") && (!effect["mode"].isString() || (effect["mode"] != "guarded" && effect["mode"] != "idempotent"))) return fail("effect.mode must be guarded or idempotent");
    if (effect.isMember("reconcile_url")) {
      if (!effect["reconcile_url"].isString()) return fail("effect.reconcile_url must be a URL string");
      const auto url = effect["reconcile_url"].asString();
      if (url.empty() || url.size() > 4096 || url.find('#') != std::string::npos ||
          origin(url).empty() || origin(url) != origin(spec.get("url","").asString())) return fail("effect.reconcile_url must use the write's exact origin and have no fragment");
    }
  }
  if (method == "post" && spec["headers"].isObject()) {
    std::unordered_set<std::string> names;
    for (const auto& name : spec["headers"].getMemberNames()) {
      const auto key = lower_copy(trim_copy(name));
      if (!names.insert(key).second) return fail("duplicate HTTP header name ignoring case");
      if (key == "idempotency-key" || key == "x-agent-effect-hash" || key == "x-agent-effect-attempt" || key == "x-agent-effect-protocol") return fail("effect identity headers are server-owned");
    }
  }
  return true;
}

Json::Value workflow_effect_execute(const WorkflowEffectContext& context, const Json::Value& spec,
                                    const EffectDispatch& dispatch, const EffectReconcile& reconcile) {
  std::string error;
  const auto cancelled = [&]() { return context.should_cancel && context.should_cancel(); };
  const auto cancelled_result = [&]() { auto out=failure("cancelled"); out["cancelled"]=true; return out; };
  if (cancelled()) return cancelled_result();
  if (!workflow_effect_validate_http(spec,&error)) return failure(error);
  if (!context.db || !context.db->is_open() || context.workflow_id.empty() || context.task_id.empty()) return failure("durable effect context unavailable; write refused");
  AgentDb::WorkflowEffectRow definition;
  definition.workflow_id = context.workflow_id; definition.task_id = context.task_id;
  Json::Value identity(Json::arrayValue); identity.append(context.workflow_id); identity.append(context.task_id);
  const auto identity_hash = hash(identity);
  if (identity_hash.empty()) return failure("cannot hash effect identity");
  definition.effect_id = "fx_" + identity_hash.substr(7);
  Json::Value request(Json::objectValue);
  request["protocol"] = "agentd.effect.v1";
  for (const auto& field : {"url","body","bearer_env"}) if (spec.isMember(field)) request[field] = spec[field];
  request["method"] = "POST";
  request["headers"] = Json::Value(Json::objectValue);
  if (spec["headers"].isObject()) for (const auto& name : spec["headers"].getMemberNames()) request["headers"][lower_copy(name)] = spec["headers"][name];
  definition.request_hash = hash(request);
  definition.mode = spec["effect"].isObject() ? spec["effect"].get("mode","guarded").asString() : "guarded";
  definition.reconcile_url = spec["effect"].isObject() ? spec["effect"].get("reconcile_url","").asString() : "";
  if (definition.request_hash.empty()) return failure("cannot hash effect request");
  AgentDb::WorkflowEffectRow row;
  if (!context.db->prepare_workflow_effect(definition,&row,&error)) return failure(error);
  auto transition = [&](const std::string& state,const std::string& receipt = "") {
    AgentDb::WorkflowEffectRow updated;
    if (!context.db->transition_workflow_effect(row,state,receipt,&updated,&error)) return false;
    row = std::move(updated); return true;
  };

  if (row.state == "completed") {
    Json::Value receipt;
    if (!json_parse_any(row.receipt_json,&receipt,&error) || !receipt.isObject()) return failure("invalid durable effect receipt; refusing redispatch");
    if (row.mode == "guarded" || receipt["ok"] == true || receipt["retryable"] != true) {
      receipt["retryable"] = false;
      metadata(&receipt,row,true);
      return receipt;
    }
  }
  if (row.state == "dispatching" || row.state == "unknown") {
    const auto resolver = row.reconcile_url;
    if (!resolver.empty()) {
      auto query = spec;
      query.removeMember("effect"); query.removeMember("body"); query["method"] = "GET";
      query["timeout_ms"] = Json::Int64(10000); query["max_bytes"] = Json::Int64(512*1024);
      query["url"] = resolver + (resolver.find('?') == std::string::npos ? "?" : "&") +
        "effect_id=" + row.effect_id + "&attempt=" + std::to_string(row.attempt) + "&request_hash=" + row.request_hash;
      const auto result = reconcile(query);
      const auto& proof = result["http"]["response_json"];
      const bool matches = result["ok"] == true && proof.isObject() && proof["schema"] == "agentd.effect.receipt.v1" && proof["effect_id"] == row.effect_id &&
        proof["request_hash"] == row.request_hash && proof["attempt"].isInt64() && proof["attempt"].asInt64() == row.attempt && proof["final"] == true;
      if (matches && proof["state"] == "committed" && proof["result"].isObject() &&
          proof["result"]["status"].isInt() && proof["result"]["status"].asInt() >= 200 && proof["result"]["status"].asInt() < 300 && proof["result"].isMember("response_json")) {
        Json::Value out(Json::objectValue);
        out["ok"] = true; out["assistant_text"] = "http_json outcome reconciled";
        out["http"]["status"] = proof["result"]["status"];
        out["http"]["transport_ok"] = true;
        out["http"]["response_json"] = proof["result"]["response_json"];
        out["http"]["response_text"] = encode(proof["result"]["response_json"]);
        const auto response_hash = hash(proof["result"]["response_json"]);
        if (!response_hash.empty()) {
          out["http"]["response_sha256"] = response_hash;
          out["http"]["response_sha256_alg"] = "agent_json_c14n_v1";
        }
        out["retryable"] = false;
        if (!transition("completed",encode(out))) return failure(error);
        metadata(&out,row,true); out["effect"]["reconciled"] = true;
        return out;
      }
      if (matches && proof["state"] == "not_applied") {
        // The adapter contract requires final closure of this attempt: an
        // absent lookup alone is not proof that an in-flight write cannot commit.
        if (!transition("prepared")) return failure(error);
      } else if (row.mode != "idempotent") {
        const bool pending = result["retryable"] == true ||
          (proof.isObject() && proof["schema"] == "agentd.effect.receipt.v1" && proof["state"] == "pending" && proof["effect_id"] == row.effect_id &&
           proof["request_hash"] == row.request_hash && proof["attempt"].isInt64() && proof["attempt"].asInt64() == row.attempt);
        return unresolved(row,pending ? "reconciliation is pending; no write was repeated" : "reconciliation did not establish a matching terminal outcome",pending);
      }
    } else if (row.mode != "idempotent") {
      return unresolved(row,"prior dispatch has no durable response and no reconciliation adapter is configured",false);
    }
  }

  if (cancelled()) return cancelled_result();
  if (!transition("dispatching")) return failure(error,true);
  const std::map<std::string,std::string> identity_headers = {
    {"Idempotency-Key",row.effect_id}, {"X-Agent-Effect-Hash",row.request_hash}, {"X-Agent-Effect-Attempt",std::to_string(row.attempt)},
    {"X-Agent-Effect-Protocol","agentd.effect.v1"}
  };
  auto out = dispatch(identity_headers);
  const bool transport_ok = out.isObject() && out["http"].isObject() && out["http"]["transport_ok"] == true;
  if (!out.isObject()) out = failure("invalid HTTP dispatch result");
  if (row.mode == "guarded" && transport_ok) out["retryable"] = false;
  if (!transition(transport_ok ? "completed" : "unknown",encode(out))) return unresolved(row,"could not durably settle dispatch receipt: " + error,false);
  if (!transport_ok && row.mode == "guarded") {
    const bool can_reconcile = !row.reconcile_url.empty();
    out = unresolved(row,"HTTP outcome is unknown: " + out.get("error","response unavailable").asString(),can_reconcile);
  }
  metadata(&out,row);
  return out;
}
}  // namespace agentd
