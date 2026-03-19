#pragma once

#include "session_voice_signal_protocol.h"

#include <functional>
#include <string>

namespace agentd {

using VoiceBrokerSignalEventCallback = std::function<bool(const VoiceBrokerSignalEvent&)>;

bool send_voice_broker_signal(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& type,
  const Json::Value& payload,
  std::string* out_err
);

bool send_voice_broker_answer(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalDescription& answer,
  std::string* out_err
);

bool send_voice_broker_bye(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalBye& bye,
  std::string* out_err
);

bool stream_voice_broker_signal_events(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  const VoiceBrokerSignalEventCallback& on_event,
  long* out_http_status,
  std::string* out_err
);

bool wait_for_voice_broker_signal_type(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& expected_type,
  int64_t timeout_ms,
  VoiceBrokerSignalEvent* out_event,
  long* out_http_status,
  std::string* out_err
);

}  // namespace agentd
