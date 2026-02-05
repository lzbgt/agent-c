#pragma once

#include "daemon_config.h"

#include <string>

namespace agentd {

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
