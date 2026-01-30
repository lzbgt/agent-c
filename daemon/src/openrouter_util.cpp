#include "openrouter_util.h"

#include <string>

namespace agentd {

double pricing_to_per_million(const Json::Value& v) {
  // OpenRouter pricing fields are strings like "0.000000075" USD per token.
  // Convert to USD per 1M tokens.
  double per_token = 0.0;
  if (v.isString()) {
    try {
      per_token = std::stod(v.asString());
    } catch (...) {
      per_token = 0.0;
    }
  } else if (v.isNumeric()) {
    per_token = v.asDouble();
  }
  return per_token * 1'000'000.0;
}

bool model_supports_tools(const Json::Value& model) {
  const auto& sp = model["supported_parameters"];
  if (!sp.isArray()) return false;
  for (const auto& x : sp) {
    if (x.isString() && x.asString() == "tools") return true;
  }
  return false;
}

bool model_has_multimodal_input(const Json::Value& model) {
  const auto& arch = model["architecture"];
  if (!arch.isObject()) return false;
  const auto& inputs = arch["input_modalities"];
  if (!inputs.isArray()) return false;
  for (const auto& m : inputs) {
    if (!m.isString()) continue;
    const std::string s = m.asString();
    if (s == "image" || s == "audio" || s == "video") return true;
  }
  return false;
}

}  // namespace agentd

