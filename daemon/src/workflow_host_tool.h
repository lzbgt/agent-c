#pragma once

#include "workflow_effect.h"
#include <string>

namespace agentd {
struct DaemonConfig;
bool workflow_host_tool_validate(const Json::Value& spec, std::string* error);
Json::Value workflow_host_tool_execute(const DaemonConfig& cfg,
    const WorkflowEffectContext& context, const Json::Value& spec,
    const std::string& sessions_root, const std::string& session_id,
    int64_t elapsed_grant_ms = 0);
}  // namespace agentd
