#pragma once

#include "session_voice_start_plan.h"

#include <string>

namespace agentd {

struct VoicePeerBrokerSessionBinding {
  std::string broker_session_id;
  bool managed_broker_session = false;
};

bool resolve_voice_peer_broker_session(
  const VoicePeerStartPlan& start_plan,
  VoicePeerBrokerSessionBinding* out_binding,
  int* out_http_status,
  std::string* out_err
);

bool release_managed_voice_peer_broker_session(
  const VoicePeerStartPlan& start_plan,
  const VoicePeerBrokerSessionBinding& binding,
  std::string* out_err
);

}  // namespace agentd
