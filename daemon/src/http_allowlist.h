#pragma once

#include "daemon_config.h"

#include <string>

namespace agentd {

// Returns true if the given URL target host[:port] is allowed under cfg.workflow_http_allow_hosts.
// If the allowlist is empty, this returns true.
bool workflow_http_url_is_allowed(
  const DaemonConfig& cfg,
  const std::string& url,
  std::string* out_reason
);

}  // namespace agentd

