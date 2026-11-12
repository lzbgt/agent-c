#include "workflow_model_effect.h"
#include "agent_db_sqlite.h"
#include "agent/json_c14n.h"
#include "json_util.h"
#include "tool_loop.h"
#include "openai_tool_provider.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace agentd {
namespace {
constexpr size_t kMaxToolReceipt = 2 * 1024 * 1024;
constexpr size_t kMaxRunReceipt = 8 * 1024 * 1024;
constexpr size_t kMaxRecords = 512;
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return Json::writeString(writer, value);
}
std::string hash(const Json::Value& value) {
  const auto input = encode(value);
  if (input.size() > kMaxRunReceipt) return {};
  char digest[80] = {}, error[160] = {};
  if (agent_json_c14n_sha256_token(input.data(), input.size(), digest, error, sizeof(error)) != AGENT_OK) return {};
  return digest;
}
std::string string(const agent_string_t& value) {
  return value.data ? std::string(value.data, value.len) : std::string();
}
size_t add_limit(size_t value, size_t replayed) {
  return value > std::numeric_limits<size_t>::max() - replayed ? std::numeric_limits<size_t>::max() : value + replayed;
}
Json::Value provider_request(const agent_tool_provider_request_t& req) {
  Json::Value value;
  value["model"] = req.model ? req.model : "";
  value["force_tool"] = req.force_tool_or_null ? req.force_tool_or_null : "";
  value["step"] = Json::UInt64(req.step); value["epoch"] = Json::UInt64(req.epoch);
  auto& messages = value["messages"]; messages = Json::Value(Json::arrayValue);
  if (req.message_count > 20000 || (req.message_count && !req.messages)) return Json::Value();
  size_t bytes = 0;
  for (size_t i = 0; i < req.message_count; ++i) {
    const auto& m = req.messages[i];
    if (m.content_len > kMaxRunReceipt || (m.content_len && !m.content) || m.tool_call_count > 1024 ||
        (m.tool_call_count && !m.tool_calls)) return Json::Value();
    bytes += m.content_len;
    if (bytes > kMaxRunReceipt) return Json::Value();
    Json::Value message;
    message["role"] = int(m.role);
    message["content"] = m.content ? std::string(m.content, m.content_len) : "";
    message["name"] = m.name ? m.name : ""; message["tool_call_id"] = m.tool_call_id ? m.tool_call_id : "";
    message["tool_calls"] = Json::Value(Json::arrayValue);
    for (size_t j = 0; j < m.tool_call_count; ++j) {
      const auto& c = m.tool_calls[j]; Json::Value call;
      call["id"] = c.id ? c.id : ""; call["name"] = c.name ? c.name : "";
      call["arguments"] = c.arguments_json ? c.arguments_json : "";
      message["tool_calls"].append(call);
    }
    messages.append(message);
  }
  value["tools"] = Json::Value(Json::arrayValue);
  const size_t count = req.tools ? agent_tool_registry_count(req.tools) : 0;
  if (count > 4096) return Json::Value();
  for (size_t i = 0; i < count; ++i) {
    agent_tool_def_view_t t{};
    if (agent_tool_registry_get(req.tools, i, &t) != AGENT_OK) return Json::Value();
    Json::Value tool; tool["name"] = t.name ? t.name : "";
    tool["description"] = t.description ? t.description : "";
    tool["parameters"] = t.parameters_json ? t.parameters_json : "";
    value["tools"].append(tool);
  }
  return value;
}
bool response_value(const agent_tool_provider_response_t& response, Json::Value* out) {
  if (!out || (response.assistant_content.len && !response.assistant_content.data) ||
      (response.error_message.len && !response.error_message.data) ||
      response.assistant_content.len > kMaxToolReceipt || response.error_message.len > kMaxToolReceipt ||
      response.tool_call_count > 1024 || (response.tool_call_count && !response.tool_calls)) return false;
  (*out)["assistant"] = string(response.assistant_content);
  (*out)["error"] = string(response.error_message);
  (*out)["calls"] = Json::Value(Json::arrayValue);
  for (size_t i = 0; i < response.tool_call_count; ++i) {
    const auto& c = response.tool_calls[i];
    if (!c.name.data || !c.name.len || c.name.len > 1024 || c.id.len > 4096 || c.arguments_json.len > kMaxToolReceipt ||
        (c.id.len && !c.id.data) || (c.arguments_json.len && !c.arguments_json.data)) return false;
    Json::Value call;
    call["id"] = string(c.id); call["name"] = string(c.name); call["arguments"] = string(c.arguments_json);
    (*out)["calls"].append(call);
  }
  return encode(*out).size() <= kMaxToolReceipt;
}
bool valid_prefetch(const Json::Value& value) {
  if (!value.isObject() || !value["text"].isString() || !value["error"].isString() ||
      !value["http_status"].isInt64() || value["http_status"].asInt64() < 0 ||
      value["http_status"].asInt64() > 599 || !value["usage"].isObject() ||
      encode(value).size() > kMaxToolReceipt) return false;
  if (value["error"].asString().empty() &&
      (value["text"].asString().empty() || value["http_status"].asInt64() < 200 || value["http_status"].asInt64() >= 300))
    return false;
  for (const auto& name : value["usage"].getMemberNames()) {
    if ((name != "prompt_tokens" && name != "completion_tokens" && name != "total_tokens") ||
        !value["usage"][name].isInt64() || value["usage"][name].asInt64() < 0) return false;
  }
  return true;
}
bool valid_response(const Json::Value& value) {
  if (!value.isObject() || !value["assistant"].isString() || !value["error"].isString() ||
      !value["calls"].isArray() || value["calls"].size() > 1024 || encode(value).size() > kMaxToolReceipt) return false;
  for (const auto& c : value["calls"]) {
    if (!c.isObject() || !c["id"].isString() || !c["name"].isString() || c["name"].asString().empty() ||
        !c["arguments"].isString()) return false;
  }
  return true;
}
std::string reasoning_key(const Json::Value& response) {
  std::string key;
  for (const auto& call : response["calls"]) {
    const auto id = call["id"].asString();
    if (!id.empty()) { if (!key.empty()) key += "|"; key += id; }
  }
  return key;
}
bool valid_adapter_state(const Json::Value& state) {
  return state.isObject() && state.size() <= 1 &&
    (state.empty() || (state["reasoning"].isString() && state["reasoning"].asString().size() <= kMaxToolReceipt));
}
Json::Value adapter_state(const OpenAIToolProviderCtx* context, const Json::Value& response) {
  Json::Value state(Json::objectValue);
  if (context) {
    const auto found = context->reasoning_by_tool_call_ids.find(reasoning_key(response));
    if (found != context->reasoning_by_tool_call_ids.end()) state["reasoning"] = found->second;
  }
  return state;
}
bool restore_adapter_state(OpenAIToolProviderCtx* context, const Json::Value& response, const Json::Value& state) {
  if (!valid_adapter_state(state)) return false;
  if (state.isMember("reasoning")) {
    const auto key = reasoning_key(response);
    if (!context || key.empty()) return false;
    context->reasoning_by_tool_call_ids[key] = state["reasoning"].asString();
  }
  return true;
}
agent_status_t restore_response(const Json::Value& value, agent_tool_provider_response_t* out) {
  if (!out || !valid_response(value)) return AGENT_ERR_INVALID_ARGUMENT;
  *out = {};
  auto copy = [](agent_string_t* dest, const Json::Value& v) {
    const auto text = v.asString(); return agent_string_set_copy(dest, text.data(), text.size());
  };
  if (copy(&out->assistant_content, value["assistant"]) != AGENT_OK || copy(&out->error_message, value["error"]) != AGENT_OK) {
    agent_tool_provider_response_free(out); return AGENT_ERR_OOM;
  }
  const auto count = value["calls"].size();
  if (count) {
    out->tool_calls = static_cast<agent_tool_call_t*>(agent_malloc(count * sizeof(agent_tool_call_t)));
    if (!out->tool_calls) { agent_tool_provider_response_free(out); return AGENT_ERR_OOM; }
    std::memset(out->tool_calls, 0, count * sizeof(agent_tool_call_t)); out->tool_call_count = count;
    for (Json::ArrayIndex i = 0; i < count; ++i) {
      const auto& c = value["calls"][i]; auto& dest = out->tool_calls[i];
      if (copy(&dest.id, c["id"]) != AGENT_OK || copy(&dest.name, c["name"]) != AGENT_OK ||
          copy(&dest.arguments_json, c["arguments"]) != AGENT_OK) {
        agent_tool_provider_response_free(out); return AGENT_ERR_OOM;
      }
    }
  }
  return AGENT_OK;
}
bool terminal_tool(const std::string& name, const std::string& raw) {
  Json::Value receipt; std::string error;
  if (raw.size() > kMaxToolReceipt || !json_parse_any(raw, &receipt, &error) || !receipt.isObject() || !receipt["ok"].isBool()) return false;
  const auto& data = receipt["data"];
  return !(receipt["cancelled"] == true || receipt["timed_out"] == true ||
    (data.isObject() && (data["cancelled"] == true || data["timed_out"] == true ||
     ((name == "shell_exec" || name == "proc_exec") && data["exit_code"].isInt() && data["exit_code"].asInt() < 0))));
}
}
WorkflowModelEffect::WorkflowModelEffect(WorkflowEffectContext context, const std::string& task_request_json)
    : context_(std::move(context)) {
  definition_.workflow_id = context_.workflow_id; definition_.task_id = context_.task_id; definition_.mode = "guarded";
  Json::Value identity(Json::arrayValue);
  identity.append(context_.workflow_id); identity.append(context_.task_id);
  const auto digest = hash(identity);
  if (!digest.empty()) definition_.effect_id = "mfx_" + digest.substr(7);
  Json::Value task, request;
  if (!json_parse_any(task_request_json, &task, &identity_error_) || !task.isObject()) {
    identity_error_ = "invalid persisted model task request"; return;
  }
  request["protocol"] = "agentd.model_run.v1"; request["request"] = task;
  definition_.request_hash = hash(request);
  if (definition_.effect_id.empty() || definition_.request_hash.empty()) identity_error_ = "cannot hash model effect identity";
}
bool WorkflowModelEffect::cancelled() const { return context_.should_cancel && context_.should_cancel(); }
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
  if (session_bound_) {
    effect["session_id"] = session_.session_id; effect["session_revision"] = Json::Int64(session_.revision);
    effect["session_committed"] = session_committed_;
  }
  if (session_conflict_) effect["session_conflict"] = true;
  if (transcript_ || restored_) {
    effect["checkpoint"] = "provider_tool_v1";
    effect["replayed_provider_calls"] = Json::UInt64(replayed_providers_);
    effect["replayed_tool_calls"] = Json::UInt64(replayed_tools_);
    effect["prefetch_calls"] = Json::Int64(prefetch_calls_);
    effect["replayed_prefetch_calls"] = Json::UInt64(replayed_prefetch_);
    effect["resume_available"] = row_.state == "dispatching" && pending_.isNull() && uncertain_.empty() && refusal_.isNull();
  }
}
Json::Value WorkflowModelEffect::unresolved(const std::string& reason) const {
  auto out = failure(reason); out["needs_input"] = true;
  auto& blocker = out["blocker"];
  blocker["id"] = row_.effect_id.empty() ? definition_.effect_id : row_.effect_id;
  blocker["kind"] = "model_run_outcome_unknown"; blocker["reason"] = reason;
  blocker["required_action"] = "Inspect the original model run, its tool receipts and downstream effects. "
    "Do not rerun it or create a replacement task while prior effects may still commit. "
    "Unreceipted calls and invalid checkpoints have no automatic resume or receipt resolver.";
  if (session_conflict_) {
    blocker["kind"] = "model_session_changed";
    blocker["required_action"] = "Inspect the newer conversation and this task's preserved tool receipts. "
      "The newer conversation was not overwritten and prior tools were not repeated. "
      "No automatic rebase or session-reset permission is implied.";
  }
  blocker["workflow_id"] = context_.workflow_id; blocker["task_id"] = context_.task_id;
  blocker["attempts"] = Json::Value(Json::arrayValue);
  blocker["attempts"].append("Inspected the durable model-run dispatch/receipt boundary; no tool was repeated.");
  blocker["attempts"].append(reason);
  annotate(&out); return out;
}
bool WorkflowModelEffect::restore_checkpoint(const Json::Value& value) {
  if (!value.isObject() || value["schema"] != "agentd.model_run.checkpoint.v1" ||
      !value["config_hash"].isString() || value["config_hash"].asString().empty() ||
      !value["records"].isArray() || value["records"].size() > kMaxRecords ||
      !value.isMember("pending") || !value["pending"].isNull() || !value["checksum"].isString()) return false;
  auto checked = value; const auto checksum = checked["checksum"].asString(); checked.removeMember("checksum");
  if (checksum.empty() || hash(checked) != checksum) return false;
  for (Json::ArrayIndex i = 0; i < value["records"].size(); ++i) {
    const auto& record = value["records"][i];
    if (!record.isObject() || !record["request_hash"].isString() || record["request_hash"].asString().empty()) return false;
    if (record["kind"] == "provider") {
      if (!record["status"].isInt() || (record["status"].asInt() != AGENT_OK && record["status"].asInt() != AGENT_ERR_CONTEXT_TOO_LONG) ||
          !valid_response(record["response"]) || !valid_adapter_state(record["adapter_state"]) ||
          encode(record).size() > kMaxToolReceipt) return false;
    } else if (record["kind"] == "vision_prefetch") {
      if (i != 0 || !valid_prefetch(record["result"])) return false;
    } else if (record["kind"] == "tool") {
      if (!record["name"].isString() || !record["result"].isString() ||
          !terminal_tool(record["name"].asString(), record["result"].asString())) return false;
    } else return false;
  }
  if (value.isMember("session")) {
    const auto& session = value["session"];
    if (!session.isObject() || !session["id"].isString() || session["id"].asString().empty() ||
        !session["revision"].isInt64() || session["revision"].asInt64() < 0 ||
        session["revision"].asInt64() >= 9007199254740991LL || value["session_conflict"] == true) return false;
    session_.session_id = session["id"].asString(); session_.revision = session["revision"].asInt64();
    session_bound_ = true;
  }
  records_ = value["records"]; config_hash_ = value["config_hash"].asString();
  restored_ = true; dispatched_ = true; return true;
}
Json::Value WorkflowModelEffect::preflight() {
  checked_ = false;
  if (cancelled()) { auto out = failure("cancelled"); out["cancelled"] = true; return out; }
  if (!identity_error_.empty()) return failure(identity_error_);
  if (!context_.db || !context_.db->is_open() || context_.workflow_id.empty() || context_.task_id.empty())
    return failure("durable model effect context unavailable; run refused");
  bool found = false; std::string error;
  if (!context_.db->get_workflow_effect(definition_.effect_id, &row_, &found, &error)) return failure(error, true);
  if (!found) { checked_ = true; return Json::Value(); }
  if (row_.workflow_id != definition_.workflow_id || row_.task_id != definition_.task_id ||
      row_.request_hash != definition_.request_hash || row_.mode != "guarded")
    return unresolved("model effect identity/request mismatch; changed run refused");
  if (row_.state == "prepared") { checked_ = true; return Json::Value(); }
  Json::Value receipt;
  const bool parsed = row_.receipt_json.size() <= kMaxRunReceipt && json_parse_any(row_.receipt_json, &receipt, &error) && receipt.isObject();
  if (row_.state == "dispatching" && parsed && restore_checkpoint(receipt)) { checked_ = true; return Json::Value(); }
  if (row_.state != "completed") {
    session_conflict_ = parsed && receipt["session_conflict"] == true;
    return unresolved(session_conflict_ ? "session conversation changed; saved tool receipts retained without replay" :
      "model run has an unreceipted host dispatch or unusable checkpoint; no provider or tool was repeated");
  }
  if (!parsed || receipt["schema"] != "agentd.model_run.receipt.v1" || !receipt["result"].isObject() || receipt["result"]["ok"] != true)
    return unresolved("saved model-run receipt is invalid; refusing redispatch");
  auto out = receipt["result"]; out["retryable"] = false;
  if (out["effect"].isMember("prefetch_calls")) {
    out["effect"]["original_usage"]["prefetch_calls"] = out["effect"]["prefetch_calls"];
    out["effect"]["prefetch_calls"] = Json::Int64(0);
  }
  annotate(&out, true);
  for (const char* key : {"tool_calls_total", "steps_executed", "elapsed_ms", "prompt_tokens", "completion_tokens", "total_tokens"}) {
    if (out.isMember(key)) out["effect"]["original_usage"][key] = out[key];
    out[key] = Json::Int64(0);
  }
  out["tool_calls_by_tool"] = Json::Value(Json::objectValue); return out;
}
void WorkflowModelEffect::bind_scope(const Json::Value& scope) { scope_hash_ = hash(scope); }
Json::Value WorkflowModelEffect::bind_session(const std::string& id, int64_t revision) {
  if (id.empty() || revision < 0 || revision >= 9007199254740991LL ||
      (restored_ && (!session_bound_ || session_.session_id != id || session_.revision != revision))) {
    session_conflict_ = true;
    refusal_ = unresolved("session conversation changed since the saved model frontier; continuation refused");
    return refusal_;
  }
  session_ = {id, revision}; session_bound_ = true; session_configured_ = true;
  return Json::Value();
}
bool WorkflowModelEffect::validate_session() {
  if (!session_bound_) return true;
  std::string error;
  if (!session_configured_ || !context_.db->session_matches(session_, &error)) {
    session_conflict_ = error.rfind("session revision conflict", 0) == 0 || !session_configured_;
    refusal_ = unresolved(error.empty() ? "session continuation was not configured" : error);
    return false;
  }
  return true;
}
void WorkflowModelEffect::stage_session(std::vector<AgentDb::MessageRow> messages, const std::string& error) {
  size_t bytes = 0;
  for (const auto& message : messages) {
    bytes += message.role.size() + message.content.size() + message.mm_json.size();
    if (bytes > kMaxRunReceipt) break;
  }
  if (!session_bound_ || !error.empty() || messages.size() > 20000 || bytes > kMaxRunReceipt) {
    refusal_ = unresolved(error.empty() ? "session result exceeded the bounded commit contract" : error);
    return;
  }
  session_messages_ = std::move(messages); session_staged_ = true;
}
void WorkflowModelEffect::set_fresh_budget(bool tools_limited, int64_t tools, bool steps_limited, int64_t steps) {
  tools_budget_limited_ = tools_limited; tools_budget_ = std::max<int64_t>(0, tools);
  steps_budget_limited_ = steps_limited; steps_budget_ = std::max<int64_t>(0, steps);
}
void WorkflowModelEffect::configure_transcript(ToolLoopOptions* options, const Json::Value& provider_identity) {
  if (!options) return;
  transcript_ = true;
  if (session_bound_ && !session_configured_) {
    refusal_ = unresolved("saved session transcript has no current conversation snapshot");
    return;
  }
  options->provider_decorator = [](void* ptr, agent_tool_provider_t provider, OpenAIToolProviderCtx* context) {
    return static_cast<WorkflowModelEffect*>(ptr)->wrap_provider(provider, context);
  };
  options->provider_decorator_ctx = this;
  Json::Value identity = provider_identity;
  identity["scope_hash"] = scope_hash_;
  identity["adapter_state"] = "openai_reasoning_v1";
  identity["stream_assistant"] = options->stream_assistant;
  identity["max_chars"] = Json::UInt64(options->max_chars);
  identity["keep_last"] = Json::UInt64(options->keep_last_messages);
  identity["tool_result_chars"] = Json::UInt64(options->max_tool_result_chars);
  identity["force_tool"] = options->force_tool; identity["require_tool"] = options->require_tool_call;
  identity["memory_flush"] = options->memory_flush_enabled;
  identity["memory_flush_excerpt"] = Json::UInt64(options->memory_flush_max_excerpt_chars);
  identity["memory_flush_messages"] = Json::UInt64(options->memory_flush_max_messages);
  identity["memory_flush_per_message"] = Json::UInt64(options->memory_flush_per_message_chars);
  identity["insert_compaction_summary"] = options->insert_compaction_summary;
  identity["summary_preview_items"] = Json::UInt64(options->summary_preview_items);
  identity["summary_snippet_chars"] = Json::UInt64(options->summary_snippet_chars);
  identity["summary_max_chars"] = Json::UInt64(options->summary_max_chars);
  identity["max_tool_call_args_chars"] = Json::UInt64(options->max_tool_call_args_chars);
  identity["max_repeated_tool_calls"] = Json::UInt64(options->max_repeated_tool_calls);
  const auto fingerprint = hash(identity);
  if (fingerprint.empty() || (restored_ && config_hash_ != fingerprint)) {
    refusal_ = unresolved("provider or host execution context changed; saved transcript was not replayed");
    return;
  }
  config_hash_ = fingerprint;
  provider_limit_ = options->max_steps; provider_limited_ = provider_limit_ > 0;
  tool_limit_ = options->max_tool_calls_total; tool_limited_ = tool_limit_ > 0;
  if (steps_budget_limited_) {
    provider_limit_ = provider_limited_ ? std::min<size_t>(provider_limit_, steps_budget_) : size_t(steps_budget_);
    provider_limited_ = true;
  }
  if (tools_budget_limited_) {
    tool_limit_ = tool_limited_ ? std::min<size_t>(tool_limit_, tools_budget_) : size_t(tools_budget_);
    tool_limited_ = true;
  }
  per_tool_limit_ = options->max_tool_calls_per_tool;
  for (const auto& item : options->tool_call_limits) per_tool_limits_[item.tool] = item.max_calls;
  size_t providers = 0, tools = 0, max_per_tool = 0;
  std::map<std::string, size_t> historical;
  for (const auto& record : records_) {
    if (record["kind"] == "provider") ++providers;
    else if (record["kind"] == "tool") { ++tools; max_per_tool = std::max(max_per_tool, ++historical[record["name"].asString()]); }
  }
  // Core traversal includes replay; explicit adapter limits below count only
  // new calls. One sentinel slot lets a zero-grant frontier refuse before I/O.
  if (provider_limited_) options->max_steps = add_limit(std::max<size_t>(1, provider_limit_), providers);
  if (tool_limited_) options->max_tool_calls_total = add_limit(std::max<size_t>(1, tool_limit_), tools);
  if (options->max_tool_calls_per_tool) options->max_tool_calls_per_tool = add_limit(options->max_tool_calls_per_tool, max_per_tool);
  for (auto& limit : options->tool_call_limits) if (limit.max_calls) limit.max_calls = add_limit(limit.max_calls, historical[limit.tool]);
}
bool WorkflowModelEffect::vision_prefetch(const Json::Value& request,
    const std::function<Json::Value()>& invoke, Json::Value* result, bool* replayed) {
  if (replayed) *replayed = false;
  if (!result || !replayed || !invoke || !checked_ || !transcript_ || config_hash_.empty() ||
      !refusal_.isNull() || !uncertain_.empty()) return false;
  if (cancelled()) { refusal_ = failure("cancelled"); refusal_["cancelled"] = true; return false; }
  if (!request.isObject()) { reject("vision preparation request is not an object"); return false; }
  Json::Value input; input["protocol"] = "agentd.vision_prefetch.v1";
  input["config_hash"] = config_hash_; input["request"] = request;
  // The task fingerprint pins user-requested settings. Runtime remaining-token
  // clamps may shrink on retry; cached analysis needs no new output-token grant.
  input["request"].removeMember("max_completion_tokens");
  input["request"].removeMember("max_tokens");
  const auto digest = hash(input);
  if (digest.empty() || cursor_ != 0) { reject("vision preparation request or ordering cannot be checkpointed safely"); return false; }
  if (cursor_ < records_.size()) {
    const auto& record = records_[Json::ArrayIndex(cursor_)];
    if (record["kind"] != "vision_prefetch" || record["request_hash"] != digest || !valid_prefetch(record["result"])) {
      reject("vision preparation differs from saved model transcript; refusing reinterpretation"); return false;
    }
    *result = record["result"]; *replayed = true; ++cursor_; ++replayed_prefetch_; return true;
  }
  if (!validate_session()) return false;
  if (provider_limited_ && size_t(provider_calls_) >= provider_limit_) {
    refusal_ = failure("vision preparation has no fresh provider-step grant"); budget_refused_ = true; return false;
  }
  ++provider_calls_; ++prefetch_calls_;
  try { *result = invoke(); }
  catch (...) { reject("vision preparation adapter threw before a usable receipt"); return false; }
  if (!valid_prefetch(*result)) { reject("vision preparation result exceeds its bounded transcript contract"); return false; }
  Json::Value record; record["kind"] = "vision_prefetch"; record["request_hash"] = digest; record["result"] = *result;
  return append_record(std::move(record));
}
agent_tool_executor_t WorkflowModelEffect::wrap(agent_tool_executor_t base, Authorize authorize) {
  base_ = base; authorize_ = std::move(authorize); return {this, execute};
}
agent_tool_provider_t WorkflowModelEffect::wrap_provider(agent_tool_provider_t base, OpenAIToolProviderCtx* context) {
  provider_ = base; provider_context_ = context; return {this, generate};
}
agent_status_t WorkflowModelEffect::reject(const std::string& reason) {
  refusal_ = failure(reason); uncertain_ = reason; return AGENT_ERR_INTERNAL;
}
Json::Value WorkflowModelEffect::checkpoint_value() const {
  Json::Value value;
  value["schema"] = "agentd.model_run.checkpoint.v1"; value["config_hash"] = config_hash_;
  value["records"] = records_; value["pending"] = pending_;
  if (session_bound_) { value["session"]["id"] = session_.session_id; value["session"]["revision"] = Json::Int64(session_.revision); }
  if (session_conflict_) value["session_conflict"] = true;
  value["checksum"] = hash(value); return value;
}
bool WorkflowModelEffect::save_checkpoint(bool admitting_tool) {
  const auto value = checkpoint_value();
  if (value["checksum"].asString().empty()) { reject("model transcript exceeded its bounded checkpoint size"); return false; }
  if (!dispatched_) return true;
  AgentDb::WorkflowEffectRow saved; std::string error;
  if (!context_.db->checkpoint_workflow_model_effect(row_, encode(value), admitting_tool, &saved, &error, session_bound_ ? &session_ : nullptr)) {
    if (error.rfind("session revision conflict",0) == 0) {
      session_conflict_ = true;
      // This admission rolled back before any host dispatch. Preserve the
      // prior receipts without implying the rejected call might have run.
      if (admitting_tool) pending_ = Json::Value();
    }
    reject("model transcript persistence failed: " + error); return false;
  }
  row_ = std::move(saved); return true;
}
bool WorkflowModelEffect::append_record(Json::Value record) {
  if (records_.size() >= kMaxRecords) { reject("model transcript record limit reached"); return false; }
  records_.append(std::move(record)); cursor_ = records_.size();
  return save_checkpoint(false);
}
agent_status_t WorkflowModelEffect::generate(void* ptr, const agent_tool_provider_request_t* req, agent_tool_provider_response_t* out) {
  auto& self = *static_cast<WorkflowModelEffect*>(ptr);
  if (!req || !out || !self.checked_ || !self.provider_.generate || !self.refusal_.isNull() || !self.uncertain_.empty()) return AGENT_ERR_INTERNAL;
  if (self.cancelled()) return AGENT_ERR_CANCELLED;
  const auto request = provider_request(*req);
  const auto digest = request.isNull() ? std::string() : hash(request);
  if (digest.empty()) return self.reject("model provider request cannot be checkpointed safely");
  if (self.cursor_ < self.records_.size()) {
    const auto& record = self.records_[Json::ArrayIndex(self.cursor_)];
    if (record["kind"] != "provider" || record["request_hash"] != digest)
      return self.reject("model transcript request mismatch; refusing changed continuation");
    const auto status = restore_response(record["response"], out);
    if (status != AGENT_OK || !restore_adapter_state(self.provider_context_, record["response"], record["adapter_state"]))
      return self.reject("saved provider response or required adapter metadata is invalid or unavailable");
    ++self.cursor_; ++self.replayed_providers_;
    return agent_status_t(record["status"].asInt());
  }
  if (!self.validate_session()) return AGENT_ERR_INTERNAL;
  if (self.provider_limited_ && size_t(self.provider_calls_) >= self.provider_limit_) {
    self.refusal_ = self.failure("model continuation has no fresh provider-step grant");
    self.budget_refused_ = true; return AGENT_ERR_LIMIT;
  }
  ++self.provider_calls_;
  const auto status = self.provider_.generate(self.provider_.ctx, req, out);
  if (status != AGENT_OK && status != AGENT_ERR_CONTEXT_TOO_LONG) {
    self.provider_failed_ = true; return status;
  }
  Json::Value record;
  record["kind"] = "provider"; record["request_hash"] = digest; record["status"] = int(status);
  if (!response_value(*out, &record["response"])) return self.reject("provider response exceeds the bounded transcript contract");
  record["adapter_state"] = adapter_state(self.provider_context_, record["response"]);
  if (!valid_adapter_state(record["adapter_state"]) || encode(record).size() > kMaxToolReceipt)
    return self.reject("provider adapter metadata exceeds the bounded transcript contract");
  if (!self.append_record(std::move(record))) return AGENT_ERR_INTERNAL;
  self.provider_failed_ = false; return status;
}
agent_status_t WorkflowModelEffect::execute(void* ptr, const char* tool, const char* args, agent_string_t* result) {
  auto& self = *static_cast<WorkflowModelEffect*>(ptr);
  if (!self.checked_ || !self.base_.execute || !self.refusal_.isNull() || !self.uncertain_.empty()) return AGENT_ERR_INTERNAL;
  if (self.cancelled()) return AGENT_ERR_CANCELLED;
  if (self.restored_ && !self.transcript_) return self.reject("saved transcript was not configured; host dispatch refused");
  const std::string name = tool ? tool : "";
  Json::Value request; request["name"] = name; request["arguments"] = args ? args : "";
  const auto digest = hash(request);
  if (digest.empty()) return self.reject("model tool request cannot be checkpointed safely");
  if (self.transcript_ && self.cursor_ < self.records_.size()) {
    const auto& record = self.records_[Json::ArrayIndex(self.cursor_)];
    if (record["kind"] != "tool" || record["request_hash"] != digest || record["name"] != name)
      return self.reject("model tool transcript mismatch; refusing changed continuation");
    const auto raw = record["result"].asString();
    const auto status = agent_string_set_copy(result, raw.data(), raw.size());
    if (status == AGENT_OK) { ++self.cursor_; ++self.replayed_tools_; }
    return status;
  }
  if (self.authorize_) {
    const auto status = self.authorize_(tool, args);
    if (status != AGENT_OK) return status;
  }
  if (self.cancelled()) return AGENT_ERR_CANCELLED;
  const auto limit = self.per_tool_limits_.count(name) ? self.per_tool_limits_.at(name) : self.per_tool_limit_;
  if ((self.tool_limited_ && size_t(self.calls_) >= self.tool_limit_) ||
      (limit && self.calls_by_tool_.get(name, Json::UInt64(0)).asUInt64() >= limit) ||
      (self.context_.dispatch_capacity_available && !self.context_.dispatch_capacity_available())) {
    self.refusal_ = self.failure("workflow tool-call grant exhausted before model host dispatch");
    self.budget_refused_ = true; return AGENT_ERR_LIMIT;
  }
  if (self.transcript_) {
    if (self.records_.size() >= kMaxRecords) return self.reject("model transcript record limit reached before tool dispatch");
    self.pending_ = request; self.pending_["request_hash"] = digest;
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
    if (self.transcript_) {
      admission = self.checkpoint_value();
      if (admission["checksum"].asString().empty()) return self.reject("model checkpoint too large before tool dispatch");
    } else { admission["schema"] = "agentd.model_run.dispatch.v1"; admission["first_tool"] = name; }
    AgentDb::WorkflowEffectRow admitted;
    if (!self.context_.db->transition_workflow_effect(self.row_, "dispatching", encode(admission), &admitted, &error, self.session_bound_ ? &self.session_ : nullptr)) {
      if (error.rfind("session revision conflict",0) == 0) {
        self.session_conflict_ = true; self.refusal_ = self.unresolved(error);
      } else self.refusal_ = self.failure("model host dispatch was not admitted: " + error, true);
      return AGENT_ERR_INTERNAL;
    }
    self.row_ = std::move(admitted); self.dispatched_ = true;
  } else if (self.transcript_ && !self.save_checkpoint(true)) return AGENT_ERR_INTERNAL;
  ++self.calls_;
  self.calls_by_tool_[name] = Json::Int64(self.calls_by_tool_.get(name, Json::Int64(0)).asInt64() + 1);
  agent_status_t status;
  try { status = self.base_.execute(self.base_.ctx, tool, args, result); }
  catch (...) { self.uncertain_ = "model-selected host executor interrupted without a terminal result"; return AGENT_ERR_INTERNAL; }
  const bool bounded = result && result->data && result->len <= kMaxToolReceipt;
  if (status != AGENT_OK || !bounded || !terminal_tool(name, std::string(result->data, result->len))) {
    self.uncertain_ = "model-selected host tool did not return a bounded terminal result; timeout or downstream effects may be unverified";
    return AGENT_ERR_INTERNAL;
  }
  if (self.transcript_) {
    self.pending_ = Json::Value();
    Json::Value record;
    record["kind"] = "tool"; record["request_hash"] = digest; record["name"] = name;
    record["result"] = std::string(result->data, result->len);
    if (!self.append_record(std::move(record))) return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}
Json::Value WorkflowModelEffect::finish(Json::Value out) {
  if (finished_) return out;
  finished_ = true;
  checked_ = false;
  if (!out.isObject()) out = failure("invalid model run result");
  if (!refusal_.isNull()) for (const auto& key : refusal_.getMemberNames()) out[key] = refusal_[key];
  if (transcript_) {
    out["steps_executed"] = Json::Int64(provider_calls_);
    out["tool_calls_total"] = Json::Int64(calls_); out["tool_calls_by_tool"] = calls_by_tool_;
  }
  if (!dispatched_) {
    if (session_bound_ && out["ok"] == true) {
      if (cancelled()) { out = failure("cancelled"); out["cancelled"] = true; return out; }
      std::string error;
      if (!session_staged_ || !context_.db->replace_session_messages(session_.session_id, session_messages_,
            agent_db_unix_ms_now(), &error, &session_)) {
        session_conflict_ = error.rfind("session revision conflict",0) == 0;
        return unresolved(error.empty() ? "session result not staged for atomic publication" : error);
      }
      session_committed_ = true;
    }
    return out;
  }
  out["tool_calls_total"] = Json::Int64(calls_); out["tool_calls_by_tool"] = calls_by_tool_;
  const bool stopped = cancelled() || out.get("cancelled", false) == true;
  if (transcript_ && uncertain_.empty() && pending_.isNull() && !stopped &&
      cursor_ == records_.size() && refusal_["needs_input"] != true &&
      (out["ok"] != true || provider_failed_ || budget_refused_)) {
    out["ok"] = false;
    out["retryable"] = provider_failed_ && !budget_refused_;
    if (out["retryable"] == true) out["retry_in_ms"] = Json::Int64(1000);
    out.removeMember("needs_input"); out.removeMember("blocker");
    annotate(&out);
    out["effect"]["resume_available"] = true;
    return out;
  }
  const bool complete = out["ok"] == true && uncertain_.empty() && !stopped &&
    (!transcript_ || cursor_ == records_.size()) && (!session_bound_ || session_staged_);
  if (!complete) {
    const auto why = !uncertain_.empty() ? uncertain_ :
      stopped ? "model run stopped after host dispatch; prior effects must not be repeated" :
      "model run failed after host dispatch without a safe continuation boundary";
    const auto blocked = unresolved(why);
    if (out["error"].isString()) out["run_error"] = out["error"];
    for (const auto& key : blocked.getMemberNames()) out[key] = blocked[key];
    if (stopped) out["cancelled"] = true;
  }
  out["retryable"] = false; annotate(&out);
  Json::Value receipt;
  receipt["schema"] = "agentd.model_run.receipt.v1"; receipt["result"] = out;
  if (complete && session_bound_) receipt["result"]["effect"]["session_committed"] = true;
  // Keep exact receipted progress inspectable when conversation drift stops
  // automatic continuation. Unknown state never grants a transcript replay.
  if (!complete && session_conflict_ && (transcript_ || restored_)) receipt = checkpoint_value();
  const auto saved = encode(receipt); AgentDb::WorkflowEffectRow settled; std::string error;
  if (saved.size() > kMaxRunReceipt ||
      !context_.db->transition_workflow_effect(row_, complete ? "completed" : "unknown", saved, &settled, &error,
        complete && session_bound_ ? &session_ : nullptr, complete && session_bound_ ? &session_messages_ : nullptr)) {
    if (error.rfind("session revision conflict",0) == 0) {
      session_conflict_ = true;
      // The failed fenced transaction rolled back. Preserve the original
      // transcript while stopping automatic recovery, without touching messages.
      AgentDb::WorkflowEffectRow stopped_row;
      if (context_.db->transition_workflow_effect(row_, "unknown", encode(checkpoint_value()), &stopped_row, nullptr))
        row_ = std::move(stopped_row);
    }
    const auto blocked = unresolved("could not durably save model-run receipt: " +
      (saved.size() > kMaxRunReceipt ? std::string("receipt exceeds 8 MiB") : error));
    for (const auto& key : blocked.getMemberNames()) out[key] = blocked[key];
    return out;
  }
  row_ = std::move(settled); session_committed_ = complete && session_bound_;
  annotate(&out); return out;
}
}  // namespace agentd
