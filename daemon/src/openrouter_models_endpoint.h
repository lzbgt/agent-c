#pragma once

#include "agentd/http_types.h"

#include "openai_client.h"

namespace agentd {

// Handles GET /api/v1/openrouter/models after CORS + auth headers are applied.
// Writes response JSON to `resp->body` and sets `resp->status` on errors.
void handle_openrouter_models_endpoint(
  const OpenAIClientConfig& ocfg,
  bool daemon_auth_enabled,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
