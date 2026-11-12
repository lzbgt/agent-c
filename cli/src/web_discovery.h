#pragma once

#include "http_client.h"
#include "toolset_host.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host_tools_internal {

struct WebDiscoveryRequest {
  std::string query;
  size_t max_results = 5;
  int timeout_ms = 12000;
  bool fresh = false;
  // Empty means unrestricted. These ranges are documented by the SearXNG API.
  std::string time_range;
};

struct WebDiscoveryEntry {
  std::string title;
  std::string url;
  std::string snippet;
  std::vector<std::string> engines;
  double score = 0;
  // Upstream-reported, unverified publication metadata; never fetched_at.
  std::string published_at;
  std::string publication_date_source;
};

struct WebDiscoveryFailure {
  std::string engine;
  std::string error;
};

struct WebDiscoveryResult {
  bool configured = false;
  bool ok = false;
  bool partial = false;
  bool empty = false;
  bool cache_hit = false;
  bool timed_out = false;
  bool cancelled = false;
  bool blocked = false;
  long http_status = 0;
  int64_t cache_age_ms = 0;
  size_t skipped_results = 0;
  std::string backend;
  std::string error;
  std::string fetched_at;
  std::string warning;
  // "unassessed" means upstream ordering is retained; relevance is not certified.
  std::string quality = "unassessed";
  std::vector<WebDiscoveryEntry> results;
  std::vector<WebDiscoveryFailure> engine_failures;
};

using WebDiscoveryHttpRequest = decltype(&agentd::http_request);
using WebDiscoveryClock = int64_t (*)();

// Only AGENT_SEARXNG_URL enables this optional backend. It must be an HTTP(S)
// origin with an explicit literal loopback IP and port. No fallback or retry
// follows a configured-backend failure.
WebDiscoveryResult web_discover(
    const WebDiscoveryRequest& request, HostCancelCallback cancel, void* context);

// The injected transport and monotonic millisecond clock support deterministic
// fixture tests. Passing a null clock uses the real steady clock.
WebDiscoveryResult web_discover_with_transport(
    const WebDiscoveryRequest& request, WebDiscoveryHttpRequest transport,
    HostCancelCallback cancel, void* context, WebDiscoveryClock clock = nullptr);

}  // namespace host_tools_internal
