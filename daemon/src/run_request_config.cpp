#include "run_request_config.h"

#include "daemon_config.h"
#include "openai_client.h"
#include "run_provider_keys.h"

#include <json/json.h>

#include <algorithm>

namespace agentd {

OpenAIClientConfig build_run_config_from_args(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& daemon_ocfg,
  const Json::Value& args
) {
  OpenAIClientConfig run_cfg = daemon_ocfg;
  const bool base_url_explicit = args.isMember("base_url") && args["base_url"].isString();
  const bool api_key_explicit = args.isMember("api_key") && args["api_key"].isString();
  if (args.isMember("model") && args["model"].isString()) run_cfg.model = args["model"].asString();
  if (base_url_explicit) run_cfg.base_url = args["base_url"].asString();
  if (api_key_explicit) run_cfg.api_key = args["api_key"].asString();
  if (args.isMember("proxy") && args["proxy"].isString()) run_cfg.proxy_url = args["proxy"].asString();
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const long t = (long)args["timeout_ms"].asInt64();
    if (t > 0) run_cfg.timeout_ms = t;
  }
  if (args.isMember("connect_timeout_ms") && args["connect_timeout_ms"].isInt64()) {
    const long t = (long)args["connect_timeout_ms"].asInt64();
    if (t >= 0) run_cfg.connect_timeout_ms = t;
  }
  if (args.isMember("stream_idle_timeout_ms") && args["stream_idle_timeout_ms"].isInt64()) {
    const long t = (long)args["stream_idle_timeout_ms"].asInt64();
    if (t >= 0) run_cfg.stream_idle_timeout_ms = t;
  }
  if (args.isMember("max_retries") && args["max_retries"].isInt()) {
    const int r = args["max_retries"].asInt();
    run_cfg.max_retries = std::max(0, std::min(r, 8));
  }
  if (args.isMember("retry_base_ms") && args["retry_base_ms"].isInt64()) {
    const long t = (long)args["retry_base_ms"].asInt64();
    if (t >= 0) run_cfg.retry_base_ms = std::min<long>(t, 60000L);
  }
  if (args.isMember("retry_max_ms") && args["retry_max_ms"].isInt64()) {
    const long t = (long)args["retry_max_ms"].asInt64();
    if (t >= 0) run_cfg.retry_max_ms = std::min<long>(t, 60000L);
  }
  if (args.isMember("retry_jitter") &&
      (args["retry_jitter"].isDouble() || args["retry_jitter"].isInt() || args["retry_jitter"].isInt64())) {
    const double j = args["retry_jitter"].asDouble();
    run_cfg.retry_jitter = std::max(0.0, std::min(j, 1.0));
  }
  if (args.isMember("respect_retry_after") && args["respect_retry_after"].isBool()) {
    run_cfg.respect_retry_after = args["respect_retry_after"].asBool();
  }

  // Provider key fallback (framework responsibility).
  apply_provider_key_fallback(daemon_cfg, daemon_ocfg, base_url_explicit, api_key_explicit, &run_cfg);

  return run_cfg;
}

}  // namespace agentd
