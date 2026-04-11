#include "llm_usage.h"

#include "json_util.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>

namespace agentd {

bool llm_try_extract_usage_tokens_from_openai_response_body(const std::string& response_body, Json::Value* out_usage_obj) {
  if (out_usage_obj) *out_usage_obj = Json::Value(Json::nullValue);
  if (!out_usage_obj) return false;
  if (response_body.empty() || response_body[0] != '{') return false;

  Json::Value root;
  std::string perr;
  if (!json_parse_any(response_body, &root, &perr) || !root.isObject()) return false;
  if (!root.isMember("usage") || !root["usage"].isObject()) return false;
  const Json::Value u = root["usage"];

  auto get_i64_nonneg = [&](const char* k, int64_t* out_v) -> bool {
    if (!out_v || !k) return false;
    *out_v = 0;
    if (!u.isMember(k)) return false;
    const Json::Value& v = u[k];
    if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return false;
    const int64_t n = v.asInt64();
    if (n < 0) return false;
    *out_v = n;
    return true;
  };

  int64_t prompt = 0;
  int64_t completion = 0;
  int64_t total = 0;
  const bool have_prompt = get_i64_nonneg("prompt_tokens", &prompt);
  const bool have_completion = get_i64_nonneg("completion_tokens", &completion);
  const bool have_total = get_i64_nonneg("total_tokens", &total);
  if (!have_prompt && !have_completion && !have_total) return false;
  if (total <= 0 && (prompt > 0 || completion > 0)) total = prompt + completion;

  Json::Value out(Json::objectValue);
  if (have_prompt) out["prompt_tokens"] = (Json::Int64)prompt;
  if (have_completion) out["completion_tokens"] = (Json::Int64)completion;
  out["total_tokens"] = (Json::Int64)total;
  *out_usage_obj = out;
  return true;
}

static bool json_i64_nonneg(const Json::Value& obj, const char* key, int64_t* out_v) {
  if (out_v) *out_v = 0;
  if (!out_v || !obj.isObject() || !key) return false;
  if (!obj.isMember(key)) return false;
  const Json::Value& v = obj[key];
  if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return false;
  const int64_t n = v.asInt64();
  if (n < 0) return false;
  *out_v = n;
  return true;
}

static bool json_bool_true(const Json::Value& obj, const char* key) {
  return obj.isObject() && key && obj.isMember(key) && obj[key].isBool() && obj[key].asBool();
}

static bool usage_tokens_from_event_data(
  const Json::Value& data,
  int64_t* out_prompt,
  int64_t* out_completion,
  int64_t* out_total
) {
  if (out_prompt) *out_prompt = 0;
  if (out_completion) *out_completion = 0;
  if (out_total) *out_total = 0;
  if (!data.isObject()) return false;

  const auto& usage = data.isMember("usage") && data["usage"].isObject() ? data["usage"] : data;
  if (!usage.isObject()) return false;

  int64_t p = 0, c = 0, t = 0;
  const bool have_p = json_i64_nonneg(usage, "prompt_tokens", &p);
  const bool have_c = json_i64_nonneg(usage, "completion_tokens", &c);
  const bool have_t = json_i64_nonneg(usage, "total_tokens", &t);
  if (!have_p && !have_c && !have_t) return false;
  if (t <= 0 && (p > 0 || c > 0)) t = p + c;

  if (out_prompt) *out_prompt = p;
  if (out_completion) *out_completion = c;
  if (out_total) *out_total = t;
  return true;
}

static std::string usage_event_reconcile_key(const Json::Value& data) {
  auto key_i64 = [&](const char* key, int64_t* out) -> bool {
    return json_i64_nonneg(data, key, out);
  };

  int64_t step = 0, epoch = 0, attempt = 0;
  const bool have_step = key_i64("step", &step);
  const bool have_epoch = key_i64("epoch", &epoch);
  const bool have_attempt = key_i64("attempt", &attempt);

  // Tool-loop provider events identify each LLM call by step/epoch. The direct
  // tools=none path identifies retry attempts by attempt. Fall back to a run-level
  // bucket for older events that lack both shapes.
  if (have_step || have_epoch) {
    return std::string("step:") + (have_step ? std::to_string(step) : "-") +
      ":epoch:" + (have_epoch ? std::to_string(epoch) : "-");
  }
  if (have_attempt) {
    return std::string("attempt:") + std::to_string(attempt);
  }
  return "run";
}

void llm_sum_usage_from_events(const Json::Value& events_out, int64_t* out_prompt, int64_t* out_completion, int64_t* out_total) {
  if (out_prompt) *out_prompt = 0;
  if (out_completion) *out_completion = 0;
  if (out_total) *out_total = 0;
  if (!events_out.isArray()) return;

  auto sat_add = [](int64_t a, int64_t b) -> int64_t {
    if (b <= 0) return a;
    if (a > (INT64_MAX - b)) return INT64_MAX;
    return a + b;
  };

  int64_t prompt = 0;
  int64_t completion = 0;
  int64_t total = 0;
  std::set<std::string> authoritative_usage_keys;

  for (Json::ArrayIndex i = 0; i < events_out.size(); i++) {
    const auto& ev = events_out[i];
    if (!ev.isObject()) continue;
    if (!ev.isMember("type") || !ev["type"].isString()) continue;
    if (ev["type"].asString() != "llm_usage") continue;
    const auto& data = ev.isMember("data") && ev["data"].isObject() ? ev["data"] : Json::Value(Json::nullValue);
    if (!data.isObject()) continue;
    int64_t p = 0, c = 0, t = 0;
    if (!usage_tokens_from_event_data(data, &p, &c, &t)) continue;
    if (!json_bool_true(data, "estimated")) {
      authoritative_usage_keys.insert(usage_event_reconcile_key(data));
    }
  }

  for (Json::ArrayIndex i = 0; i < events_out.size(); i++) {
    const auto& ev = events_out[i];
    if (!ev.isObject()) continue;
    if (!ev.isMember("type") || !ev["type"].isString()) continue;
    if (ev["type"].asString() != "llm_usage") continue;
    const auto& data = ev.isMember("data") && ev["data"].isObject() ? ev["data"] : Json::Value(Json::nullValue);
    if (!data.isObject()) continue;

    int64_t p = 0, c = 0, t = 0;
    if (!usage_tokens_from_event_data(data, &p, &c, &t)) continue;
    if (json_bool_true(data, "estimated") &&
        authoritative_usage_keys.find(usage_event_reconcile_key(data)) != authoritative_usage_keys.end()) {
      continue;
    }

    prompt = sat_add(prompt, p);
    completion = sat_add(completion, c);
    total = sat_add(total, t);
  }

  if (out_prompt) *out_prompt = prompt;
  if (out_completion) *out_completion = completion;
  if (out_total) *out_total = total;
}

}  // namespace agentd
