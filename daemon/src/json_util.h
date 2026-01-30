#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {

std::string json_stringify(const Json::Value& v);

bool json_get_u64_nonneg(const Json::Value& obj, const char* key, uint64_t* out);

bool json_parse_object(const std::string& s, Json::Value* out, std::string* out_err);
bool json_parse_any(const std::string& s, Json::Value* out, std::string* out_err);

std::string json_try_extract_assistant_content_from_completion(const Json::Value& root);

}  // namespace agentd

