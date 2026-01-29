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
};

OpenAIChatResult openai_chat_completions(
  const OpenAIClientConfig& cfg,
  const struct agent_session* session
);

