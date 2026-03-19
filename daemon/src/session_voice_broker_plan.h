#pragma once

#include "session_voice_start_plan.h"

#include <json/json.h>

#include <string>

namespace agentd {

struct VoicePeerBrokerSessionPlan {
  std::string mode;
  std::string session_id;
  bool preflighted = false;
  std::string session_mode;
  std::string agent_id;
  std::string deployment_id;
  bool managed_broker_session = false;
};

VoicePeerBrokerSessionPlan make_voice_peer_broker_session_plan(
  const VoicePeerStartPlan& start_plan
);

Json::Value voice_peer_broker_session_plan_json(
  const VoicePeerBrokerSessionPlan& plan
);

}  // namespace agentd
