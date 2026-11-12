#pragma once

#include "agent/agent.h"
#include "http_client.h"

namespace host_tools_internal {

// Inject only the transport for offline verification. Production always uses
// the bounded native HTTP client and a fixed public search endpoint.
using WebSearchHttpRequest = decltype(&agentd::http_request);
agent_status_t web_search_execute(
    const char* arguments_json,
    agent_string_t* out_result,
    WebSearchHttpRequest request);

}  // namespace host_tools_internal
