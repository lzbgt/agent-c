#pragma once

#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

struct VoicePeerStopProcessResult {
  bool was_running = false;
  bool stopped = false;
};

struct VoicePeerManagedStopResult {
  bool was_running = false;
  bool stopped = false;
  Json::Value broker_cleanup = Json::Value(Json::objectValue);
};

bool stop_voice_peer_runtime_process(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  VoicePeerStopProcessResult* out_result,
  std::string* out_err
);

bool stop_voice_peer_runtime_with_broker_cleanup(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  const std::string& broker_token,
  VoicePeerManagedStopResult* out_result,
  std::string* out_err
);

Json::Value cleanup_managed_voice_peer_broker_session(
  const std::shared_ptr<VoicePeerRuntime>& st,
  const std::string& broker_token
);

}  // namespace agentd
