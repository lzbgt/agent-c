#pragma once

namespace Json {
class Value;
}

struct OpenAIClientConfig;

namespace agentd {

struct DaemonConfig;

OpenAIClientConfig build_run_config_from_args(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& daemon_ocfg,
  const Json::Value& args
);

}  // namespace agentd
