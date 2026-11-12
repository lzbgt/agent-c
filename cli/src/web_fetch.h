#pragma once
#include "browser_process.h"
#include "http_client.h"

namespace host_tools_internal {
using WebFetchHttpRequest = decltype(&agentd::http_request);
using WebFetchClock = int64_t (*)();
BrowserResult web_fetch_read_page(const BrowserRequest& request, HostCancelCallback cancel, void* context);
BrowserResult web_fetch_with_transport(const BrowserRequest& request, WebFetchHttpRequest http,
    BrowserReadPage browser, HostCancelCallback cancel, void* context, WebFetchClock clock = nullptr);
}  // namespace host_tools_internal
