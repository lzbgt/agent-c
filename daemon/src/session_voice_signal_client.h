#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>

namespace agentd {

struct VoiceBrokerSignalEvent {
  std::string type;
  Json::Value payload = Json::Value(Json::objectValue);
  std::string from;
  int64_t ts_unix_ms = 0;
  Json::Value raw = Json::Value(Json::objectValue);
};

using VoiceBrokerSignalEventCallback = std::function<bool(const VoiceBrokerSignalEvent&)>;

bool send_voice_broker_signal(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& type,
  const Json::Value& payload,
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

}  // namespace agentd
