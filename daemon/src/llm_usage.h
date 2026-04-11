#pragma once

#include <cstdint>
#include <string>

#include <json/json.h>

namespace agentd {

// Best-effort extraction of provider-reported token usage from an OpenAI-compatible response body.
//
// Expected response shape:
// {
//   "usage": { "prompt_tokens": N, "completion_tokens": M, "total_tokens": T }
// }
//
// Returns true only when a `usage` object exists and at least one token field is present.
// Emits `total_tokens = prompt_tokens + completion_tokens` when total_tokens is missing/0.
bool llm_try_extract_usage_tokens_from_openai_response_body(const std::string& response_body, Json::Value* out_usage_obj);

// Sums token usage across `llm_usage` events in the canonical run `events` array.
// Accepts both the current nested event data shape (`data.usage.*`) and the
// run-event schema shape (`data.*`) for compatibility.
// Token usage is best-effort and may be 0 when the provider does not surface usage.
void llm_sum_usage_from_events(const Json::Value& events_out, int64_t* out_prompt, int64_t* out_completion, int64_t* out_total);

}  // namespace agentd
