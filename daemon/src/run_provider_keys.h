#pragma once

namespace agentd {

struct DaemonConfig;
struct OpenAIClientConfig;

void apply_provider_key_fallback(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& daemon_ocfg,
  bool base_url_explicit,
  bool api_key_explicit,
  OpenAIClientConfig* run_cfg
);

}  // namespace agentd
