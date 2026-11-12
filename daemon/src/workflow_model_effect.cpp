#include "workflow_model_effect.h"
#include "agent/json_c14n.h"
#include "json_util.h"
#include <utility>

namespace agentd {
namespace {
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer, value);
}
std::string hash(const Json::Value& value) {
  const auto input = encode(value);
  char digest[80] = {}, error[160] = {};
  if (agent_json_c14n_sha256_token(input.data(), input.size(), digest, error, sizeof(error)) != AGENT_OK) return {};
  return digest;
}
constexpr size_t kMaxToolReceipt = 2 * 1024 * 1024;
constexpr size_t kMaxRunReceipt = 8 * 1024 * 1024;
}
WorkflowModelEffect::WorkflowModelEffect(WorkflowEffectContext context, const std::string& task_request_json)
    : context_(std::move(context)) {
  definition_.workflow_id = context_.workflow_id;
  definition_.task_id = context_.task_id;
  definition_.mode = "guarded";
  Json::Value identity(Json::arrayValue);
  identity.append(context_.workflow_id); identity.append(context_.task_id);
  const auto digest = hash(identity);
  if (!digest.empty()) definition_.effect_id = "mfx_" + digest.substr(7);
  Json::Value task, request;
  if (!json_parse_any(task_request_json, &task, &identity_error_) || !task.isObject()) {
    identity_error_ = "invalid persisted model task request"; return;
  }
  request["protocol"] = "agentd.model_run.v1";
  request["request"] = task; // Before dynamic budget clamps; values are hashed, not copied into the journal.
  definition_.request_hash = hash(request);
  if (definition_.effect_id.empty() || definition_.request_hash.empty()) identity_error_ = "cannot hash model effect identity";
}
bool WorkflowModelEffect::cancelled() const {
  return context_.should_cancel && context_.should_cancel();
}
Json::Value WorkflowModelEffect::failure(const std::string& reason, bool retryable) const {
  Json::Value out;
  out["ok"] = false; out["assistant_text"] = ""; out["error"] = reason; out["retryable"] = retryable;
  if (retryable) out["retry_in_ms"] = Json::Int64(1000);
  return out;
}
void WorkflowModelEffect::annotate(Json::Value* out, bool replayed) const {
  if (row_.effect_id.empty()) return;
  auto& effect = (*out)["effect"];
  effect["effect_id"] = row_.effect_id; effect["request_hash"] = row_.request_hash;
  effect["mode"] = row_.mode; effect["state"] = row_.state;
  effect["revision"] = Json::Int64(row_.revision); effect["attempt"] = Json::Int64(row_.attempt);
  effect["boundary"] = "model_run"; effect["replayed"] = replayed;
}
Json::Value WorkflowModelEffect::unresolved(const std::string& reason) const {
  auto out = failure(reason);
  out["needs_input"] = true;
  auto& blocker = out["blocker"];
  blocker["id"] = row_.effect_id.empty() ? definition_.effect_id : row_.effect_id;
  blocker["kind"] = "model_run_outcome_unknown";
  blocker["reason"] = reason;
  blocker["required_action"] = "Inspect the original model run, its tool receipts and downstream effects. "
    "Do not rerun it or create a replacement task while prior effects may still commit. "
    "This run-level guard has no automatic transcript resume or receipt resolver.";
  blocker["workflow_id"] = context_.workflow_id; blocker["task_id"] = context_.task_id;
  blocker["attempts"] = Json::Value(Json::arrayValue);
  blocker["attempts"].append("Inspected the durable model-run dispatch/receipt boundary; no tool was repeated.");
  blocker["attempts"].append(reason);
  annotate(&out);
  return out;
}
Json::Value WorkflowModelEffect::preflight() {
  checked_ = false;
  if (cancelled()) { auto out = failure("cancelled"); out["cancelled"] = true; return out; }
  if (!identity_error_.empty()) return failure(identity_error_);
  if (!context_.db || !context_.db->is_open() || context_.workflow_id.empty() || context_.task_id.empty())
    return failure("durable model effect context unavailable; run refused");
  bool found = false;
  std::string error;
  if (!context_.db->get_workflow_effect(definition_.effect_id, &row_, &found, &error)) return failure(error, true);
  if (!found) { checked_ = true; return Json::Value(); }
  if (row_.workflow_id != definition_.workflow_id || row_.task_id != definition_.task_id ||
      row_.request_hash != definition_.request_hash || row_.mode != "guarded")
    return unresolved("model effect identity/request mismatch; changed run refused");
  if (row_.state == "prepared") { checked_ = true; return Json::Value(); }
  if (row_.state != "completed") return unresolved("model run has an admitted host dispatch but no completed run receipt; no provider or tool was repeated");
  Json::Value receipt;
  if (!json_parse_any(row_.receipt_json, &receipt, &error) || !receipt.isObject() || receipt["schema"] != "agentd.model_run.receipt.v1" ||
      !receipt["result"].isObject() || receipt["result"]["ok"] != true)
    return unresolved("saved model-run receipt is invalid; refusing redispatch");
  auto out = receipt["result"];
  out["retryable"] = false;
  annotate(&out, true);
  // Historical events remain evidence, not new usage. Interrupted grants keep
  // their prior conservative charge in the workflow budget ledger.
  for (const char* key : {"tool_calls_total", "steps_executed", "elapsed_ms", "prompt_tokens", "completion_tokens", "total_tokens"}) {
    if (out.isMember(key)) out["effect"]["original_usage"][key] = out[key];
    out[key] = Json::Int64(0);
  }
  out["tool_calls_by_tool"] = Json::Value(Json::objectValue);
  return out;
}
agent_tool_executor_t WorkflowModelEffect::wrap(agent_tool_executor_t base) {
  base_ = base;
  return {this, execute};
}
agent_status_t WorkflowModelEffect::execute(void* ptr, const char* tool, const char* args, agent_string_t* result) {
  auto& self = *static_cast<WorkflowModelEffect*>(ptr);
  if (!self.checked_ || !self.base_.execute || !self.refusal_.isNull() || !self.uncertain_.empty()) return AGENT_ERR_INTERNAL;
  if (self.cancelled()) return AGENT_ERR_CANCELLED;
  if (self.context_.dispatch_capacity_available && !self.context_.dispatch_capacity_available()) {
    self.refusal_ = self.failure("workflow tool-call grant exhausted before model host dispatch");
    return AGENT_ERR_LIMIT;
  }
  if (!self.dispatched_) {
    std::string error;
    if (!self.context_.db->prepare_workflow_effect(self.definition_, &self.row_, &error)) {
      self.refusal_ = self.failure(error, true); return AGENT_ERR_INTERNAL;
    }
    if (self.row_.state != "prepared") {
      self.refusal_ = self.unresolved("another model run already admitted this task's host dispatch; no tool was repeated");
      return AGENT_ERR_INTERNAL;
    }
    Json::Value admission;
    admission["schema"] = "agentd.model_run.dispatch.v1"; admission["first_tool"] = tool ? tool : "";
    AgentDb::WorkflowEffectRow admitted;
    if (!self.context_.db->transition_workflow_effect(self.row_, "dispatching", encode(admission), &admitted, &error)) {
      self.refusal_ = self.failure("model host dispatch was not admitted: " + error, true);
      return AGENT_ERR_INTERNAL;
    }
    self.row_ = std::move(admitted); self.dispatched_ = true;
  }
  ++self.calls_;
  const std::string name = tool ? tool : "";
  self.calls_by_tool_[name] = Json::Int64(self.calls_by_tool_.get(name, Json::Int64(0)).asInt64() + 1);
  agent_status_t status;
  try { status = self.base_.execute(self.base_.ctx, tool, args, result); }
  catch (...) { self.uncertain_ = "model-selected host executor interrupted without a terminal result"; return AGENT_ERR_INTERNAL; }
  Json::Value receipt;
  std::string error;
  const bool bounded = result && result->data && result->len <= kMaxToolReceipt;
  if (status != AGENT_OK || !bounded ||
      !json_parse_any(std::string(result->data, result->len), &receipt, &error) ||
      !receipt.isObject() || !receipt["ok"].isBool()) {
    self.uncertain_ = "model-selected host tool did not return a bounded valid terminal result";
    return AGENT_ERR_INTERNAL;
  }
  const auto& data = receipt["data"];
  if (receipt["cancelled"] == true || receipt["timed_out"] == true ||
      (data.isObject() && (data["cancelled"] == true || data["timed_out"] == true ||
       ((name == "shell_exec" || name == "proc_exec") && data["exit_code"].isInt() && data["exit_code"].asInt() < 0)))) {
    self.uncertain_ = "model-selected host tool timed out or was interrupted; downstream effects are unverified";
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}
Json::Value WorkflowModelEffect::finish(Json::Value out) {
  checked_ = false;
  if (!out.isObject()) out = failure("invalid model run result");
  if (!refusal_.isNull()) for (const auto& key : refusal_.getMemberNames()) out[key] = refusal_[key];
  if (!dispatched_) return out;
  out["tool_calls_total"] = Json::Int64(calls_);
  out["tool_calls_by_tool"] = calls_by_tool_;
  const bool stopped = cancelled() || out.get("cancelled", false) == true;
  const bool complete = out["ok"] == true && uncertain_.empty() && !stopped;
  if (!complete) {
    const auto why = !uncertain_.empty() ? uncertain_ :
      stopped ? "model run stopped after host dispatch; prior effects must not be repeated" :
      "model run failed after host dispatch; prior effects must not be repeated";
    const auto blocked = unresolved(why);
    if (out["error"].isString()) out["run_error"] = out["error"];
    for (const auto& key : blocked.getMemberNames()) out[key] = blocked[key];
    if (stopped) out["cancelled"] = true;
  }
  out["retryable"] = false;
  Json::Value receipt;
  receipt["schema"] = "agentd.model_run.receipt.v1"; receipt["result"] = out;
  const auto saved = encode(receipt);
  AgentDb::WorkflowEffectRow settled;
  std::string error;
  if (saved.size() > kMaxRunReceipt ||
      !context_.db->transition_workflow_effect(row_, complete ? "completed" : "unknown", saved, &settled, &error)) {
    const auto blocked = unresolved("could not durably save model-run receipt: " +
      (saved.size() > kMaxRunReceipt ? std::string("receipt exceeds 8 MiB") : error));
    for (const auto& key : blocked.getMemberNames()) out[key] = blocked[key];
    return out;
  }
  row_ = std::move(settled);
  annotate(&out);
  return out;
}
}  // namespace agentd
