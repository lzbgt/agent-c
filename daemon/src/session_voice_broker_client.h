#pragma once

#include "daemon_config.h"

#include <string>

namespace agentd {

std::string effective_voice_broker_url(const DaemonConfig& cfg, const std::string& request_broker_url);

std::string effective_voice_broker_token(const DaemonConfig& cfg, const std::string& request_broker_token);

bool validate_voice_broker_token_if_present(const std::string& broker_token, std::string* out_err);

bool broker_create_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_agent_id,
  const std::string& broker_deployment_id,
  std::string* out_session_id,
  std::string* out_err
);

bool broker_delete_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  std::string* out_err
);

bool broker_audio_session_exists(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  bool* out_exists,
  std::string* out_mode,
  std::string* out_err
);

}  // namespace agentd
