#pragma once

#include <string>
#include <vector>

struct OpenAIClientConfig {
  std::string base_url;  // e.g. https://api.openai.com/v1
  std::string api_key;
  std::string model;
  std::string openrouter_http_referer;
  std::string openrouter_x_title;
  // Optional explicit proxy override (e.g. http://localhost:8120).
  // If empty, falls back to env (HTTPS_PROXY/https_proxy/HTTP_PROXY/http_proxy).
  std::string proxy_url;
  long timeout_ms = 60000;
  // Best-effort retries for transient network/provider failures (timeouts, 5xx, 429).
  // Note: retries can result in duplicate provider requests if the first attempt succeeded server-side
  // but the response was lost. Keep this small (default: 1).
  int max_retries = 1;
};

struct OpenAIChatResult {
  long http_status = 0;
  std::string response_body;
  std::string assistant_text;  // best-effort extracted
  std::string error_message;   // best-effort extracted (when non-2xx or parse failure)
};

struct OpenAIRawResult {
  long http_status = 0;
  std::string response_body;
};

OpenAIChatResult openai_chat_completions(
  const OpenAIClientConfig& cfg,
  const struct agent_message_view* messages,
  size_t message_count
);

OpenAIRawResult openai_chat_completions_raw(
  const OpenAIClientConfig& cfg,
  const std::string& request_body_json
);

// Minimal GET helper (host-only) with the same proxy + timeout policy as POST.
// Intended for provider metadata endpoints (e.g. OpenRouter /models).
OpenAIRawResult openai_http_get_raw(
  const OpenAIClientConfig& cfg,
  const std::string& url,
  const std::vector<std::string>& extra_headers
);

// Streaming helper for OpenAI-compatible `stream: true` responses (SSE).
// - `on_chunk` is called with each JSON chunk from `data: { ... }` SSE lines (excluding `[DONE]`).
// - `response_body` is a best-effort capture of the wire body up to `max_capture_bytes` (useful for errors).
typedef void (*OpenAIStreamChunkCallback)(void* ctx, const char* chunk_json, size_t chunk_len);

struct OpenAIStreamResult {
  long http_status = 0;
  std::string response_body;
  std::string error_message;
  bool saw_done = false;
};

OpenAIStreamResult openai_chat_completions_raw_stream(
  const OpenAIClientConfig& cfg,
  const std::string& request_body_json,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t max_capture_bytes = 256 * 1024
);

// Best-effort extraction of a human-readable provider error from an OpenAI-compatible response.
// Returns empty string if no error message could be extracted.
std::string openai_try_extract_error_message(const std::string& response_body);

// Formats a concise human-readable error summary for non-2xx responses.
// Includes extracted provider error (when available).
std::string openai_format_http_error(long http_status, const std::string& response_body);

// Best-effort heuristic for provider errors indicating the request is too large for the model/context window.
// Used to trigger "session rotation" retries with more aggressive compaction.
bool openai_is_context_too_long_error(long http_status, const std::string& response_body);
