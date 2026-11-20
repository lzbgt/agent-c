#pragma once

#include <json/json.h>
#include <string>

namespace host_tools_internal {
// Parse the machine block once, from the same captured bytes used for CAS.
bool memory_structured_parse_doc(const std::string& text, Json::Value* doc, std::string* error);
}
