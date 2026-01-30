#pragma once

#include "daemon_config.h"
#include "http_server.h"

namespace agentd {

bool daemon_auth_ok(const DaemonConfig& cfg, const HttpRequest& req);

// Returns true if authorized. If not authorized, fills resp with a 401 JSON error and returns false.
bool daemon_require_auth(const DaemonConfig& cfg, const HttpRequest& req, HttpResponse* resp);

}  // namespace agentd

