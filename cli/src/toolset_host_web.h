#pragma once
#include "agent/agent.h"
#include "browser_process.h"
#include "web_discovery.h"

namespace host_tools_internal {
using WebDiscover = decltype(&web_discover);
agent_status_t web_search_execute(const char* arguments_json, agent_string_t* out_result,
    BrowserReadPage read, HostCancelCallback cancel = nullptr, void* cancel_context = nullptr,
    WebDiscover discover = web_discover);
agent_status_t web_fetch_execute(const char* arguments_json, agent_string_t* out_result,
    BrowserReadPage read, HostCancelCallback cancel = nullptr, void* cancel_context = nullptr);
}  // namespace host_tools_internal
