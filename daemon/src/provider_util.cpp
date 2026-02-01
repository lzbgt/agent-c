#include "provider_util.h"

#include "string_util.h"

namespace agentd {

std::string provider_from_base_url(const std::string& base_url) {
  if (url_contains_ci(base_url, "deepseek")) return "deepseek";
  if (url_contains_ci(base_url, "openrouter")) return "openrouter";
  if (url_contains_ci(base_url, "moonshot")) return "moonshot";
  return "openai";
}

}  // namespace agentd
