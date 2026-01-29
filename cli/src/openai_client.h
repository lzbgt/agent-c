#pragma once

#include <string>

struct OpenAIClientConfig {
  std::string base_url;  // e.g. https://api.openai.com/v1
  std::string api_key;
  std::string model;
  std::string openrouter_http_referer;
  std::string openrouter_x_title;
  long timeout_ms = 60000;
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

// Best-effort extraction of a human-readable provider error from an OpenAI-compatible response.
// Returns empty string if no error message could be extracted.
std::string openai_try_extract_error_message(const std::string& response_body);

// Formats a concise human-readable error summary for non-2xx responses.
// Includes extracted provider error (when available).
std::string openai_format_http_error(long http_status, const std::string& response_body);
