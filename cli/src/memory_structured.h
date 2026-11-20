#pragma once

#include <json/json.h>
#include <string>

namespace host_tools_internal {
std::string memory_record_revision(const Json::Value& record);
std::string memory_strip_private_blocks(const std::string& text, int* stripped);
// Parse the machine block once, from the same captured bytes used for CAS.
bool memory_structured_parse_doc(const std::string& text, Json::Value* doc, std::string* error);
}
