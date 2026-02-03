#pragma once

#include <string>
#include <vector>

// Optional callback for surfacing provider retries to UIs/logs.
// `data_json` is a JSON object string for the event's `data` field.
using OpenAIRetryEventCallback = void (*)(void* ctx, const char* data_json);

struct OpenAIClientConfig {
  std::string base_url;  // e.g. https://api.openai.com/v1
  std::string api_key;
  std::string model;
  std::string openrouter_http_referer;
  std::string openrouter_x_title;
  // Optional explicit proxy override (e.g. http://localhost:8120).
  // If empty, falls back to env (HTTPS_PROXY/https_proxy/HTTP_PROXY/http_proxy).
  std::string proxy_url;
  // Total time budget for a request (0 means "no total timeout").
  long timeout_ms = 60000;
  // Connection establishment timeout (DNS + TCP + TLS). 0 means "derive from timeout_ms".
  long connect_timeout_ms = 0;
  // Streaming idle timeout: abort if transfer speed stays below 1 byte/sec for this many ms.
  // 0 disables the idle timeout.
  long stream_idle_timeout_ms = 0;
  // Best-effort retries for transient network/provider failures (timeouts, 5xx, 429).
  // Note: retries can result in duplicate provider requests if the first attempt succeeded server-side
  // but the response was lost. Keep this small (default: 1).
  int max_retries = 1;
  // Exponential backoff tuning for retries.
  // Attempt 0 sleep ~= retry_base_ms (subject to jitter/caps), attempt 1 doubles, etc.
  long retry_base_ms = 250;
  long retry_max_ms = 4000;
  // Multiplicative jitter in [0, 1]. 0 disables jitter.
  double retry_jitter = 0.2;
  bool respect_retry_after = true;

  // Optional hook invoked immediately before a retry sleep.
  OpenAIRetryEventCallback on_retry = nullptr;
  void* on_retry_ctx = nullptr;
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
