#pragma once

#include <json/json.h>
#include <string>

namespace agentd {

// Lowers agentd.sop.v1 into the existing workflow DAG. No execution or storage.
// On failure args is unchanged. Existing task submissions pass through unchanged.
bool workflow_compile_sop(Json::Value* args, std::string* error);

}  // namespace agentd
