#include "run_provider_keys.h"

#include "daemon_config.h"
#include "openai_client.h"
#include "provider_util.h"
#include "secrets_file.h"

#include <cstdlib>
#include <string>

namespace agentd {
namespace {

const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

}  // namespace

void apply_provider_key_fallback(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& daemon_ocfg,
  bool base_url_explicit,
  bool api_key_explicit,
  OpenAIClientConfig* run_cfg
) {
  if (!run_cfg || api_key_explicit) return;

  const std::string run_provider = provider_from_base_url(run_cfg->base_url);
  const std::string daemon_provider = provider_from_base_url(daemon_ocfg.base_url);
  const bool provider_mismatch = base_url_explicit && (run_provider != daemon_provider);

  const auto it = daemon_cfg.provider_keys.find(run_provider);
  if (it != daemon_cfg.provider_keys.end() && !it->second.empty()) {
    run_cfg->api_key = it->second;
  }

  if (!run_cfg->api_key.empty() && !provider_mismatch) return;

  std::string key;
  if (run_provider == "deepseek") {
    if (const char* k = getenv_s("DEEPSEEK_API_KEY")) key = k;
    else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
    else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) key = k3;
  } else if (run_provider == "moonshot") {
    if (const char* k = getenv_s("KIMI_API_KEY_CN")) key = k;
    else if (const char* k2 = getenv_s("MOONSHOT_API_KEY")) key = k2;
    else if (const char* k3 = getenv_s("MOONSHOT_API_KEY_CN")) key = k3;
    else if (const char* k4 = getenv_s("OPENAI_API_KEY")) key = k4;
    else if (const char* k5 = getenv_s("OPENROUTER_API_KEY")) key = k5;
    else if (const char* k6 = getenv_s("DEEPSEEK_API_KEY")) key = k6;
  } else if (run_provider == "openrouter") {
    if (const char* k = getenv_s("OPENROUTER_API_KEY")) key = k;
    else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
    else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) key = k3;
  } else {
    if (const char* k = getenv_s("OPENAI_API_KEY")) key = k;
    else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) key = k2;
    else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) key = k3;
  }

  if (key.empty()) {
    if (auto k = load_provider_key_best_effort(run_provider)) {
      key = *k;
    }
  }

  if (!key.empty()) run_cfg->api_key = key;
}

}  // namespace agentd
