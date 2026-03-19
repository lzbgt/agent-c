#pragma once

#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

struct VoicePeerBackendStartResult {
  bool ok = false;
  int http_status = 500;
  bool startup_confirmed = false;
  std::string error;
  std::shared_ptr<VoicePeerRuntime> state;
  Json::Value startup_cleanup = Json::Value(Json::nullValue);
};

}  // namespace agentd
