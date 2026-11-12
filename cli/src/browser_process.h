#pragma once

#include "toolset_host.h"
#include <json/json.h>
#include <string>
#include <cstdint>

namespace host_tools_internal {

struct BrowserRequest {
  std::string url;
  bool search = false;
  int timeout_ms = 20000;
  std::string mode = "auto";
  bool fresh = false;
  // Only native test fixtures set this; it is never accepted from tool arguments.
  bool allow_local_for_tests = false;
};

struct BrowserResult {
  bool ok = false;
  bool timed_out = false;
  bool cancelled = false;
  bool blocked = false;
  int http_status = 0;
  std::string error;
  std::string source = "local_chrome";
  bool rendered = true;
  std::string fetched_at;
  bool cache_hit = false;
  int64_t cache_age_ms = 0;
  Json::Value page;
};

bool browser_http_url(const std::string& url, bool allow_local = false);
BrowserResult browser_read_page(const BrowserRequest& request,
                               HostCancelCallback cancel, void* cancel_context);
using BrowserReadPage = decltype(&browser_read_page);

}  // namespace host_tools_internal
