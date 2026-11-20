#pragma once

#include <json/json.h>
#include <string>

namespace host_tools_internal {
std::string memory_record_revision(const Json::Value& record);
std::string memory_strip_private_blocks(const std::string& text, int* stripped);
// Decode exact stored fields, including unsupported historical schemas.
bool memory_structured_decode(const std::string &text, Json::Value *doc, std::string *error,
                              size_t *block_end = nullptr);
// Validate the supported current schema, without read-time normalization.
bool memory_structured_parse_doc(const std::string& text, Json::Value* doc, std::string* error);
}
