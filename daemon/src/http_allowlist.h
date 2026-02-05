#pragma once

#include "daemon_config.h"

#include <string>
#include <vector>

namespace agentd {

struct WorkflowHttpUrlCheck {
  // Parsed target from the URL.
  std::string host;   // lowercased; IPv6 without brackets
  int port = 0;
  bool host_is_ip = false;

  // Best-effort DNS resolution evidence used for the allow/deny decision.
  // - For literal IP targets, this includes that IP.
  // - For hostnames, this is the resolved IP list (capped).
  // Ordering: IPv4 first, then IPv6 (stable within each).
  std::vector<std::string> resolved_addrs;
};

// Checks whether the given URL is allowed by cfg.workflow_http outbound policy (allow/deny/private checks),
// and returns best-effort evidence about the parsed host/port and DNS resolution used by the decision.
bool workflow_http_url_check(
  const DaemonConfig& cfg,
  const std::string& url,
  WorkflowHttpUrlCheck* out,
  std::string* out_reason
);

// Returns true if the given URL target host[:port] is allowed under cfg.workflow_http outbound policy:
// - allow_hosts / allow_cidrs allowlists
// - deny_private defense-in-depth
// - deny_cidrs defense-in-depth
//
// If allow_hosts/allow_cidrs/deny_cidrs are all empty and deny_private is false, this returns true.
bool workflow_http_url_is_allowed(
  const DaemonConfig& cfg,
  const std::string& url,
  std::string* out_reason
);

}  // namespace agentd
