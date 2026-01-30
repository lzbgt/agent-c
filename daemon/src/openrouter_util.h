#pragma once

#include <json/json.h>

namespace agentd {

double pricing_to_per_million(const Json::Value& v);
bool model_supports_tools(const Json::Value& model);
bool model_has_multimodal_input(const Json::Value& model);

}  // namespace agentd

