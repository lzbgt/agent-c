#pragma once

#include <string>

namespace agentd {

// Best-effort provider classification from an OpenAI-compatible base URL.
// Used for selecting a provider key (deepseek/openrouter/moonshot/openai).
std::string provider_from_base_url(const std::string& base_url);

}  // namespace agentd
