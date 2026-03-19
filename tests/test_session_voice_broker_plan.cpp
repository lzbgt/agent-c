#include "session_voice_broker_plan.h"

#include <cassert>

namespace {

using agentd::VoicePeerStartPlan;
using agentd::make_voice_peer_broker_session_plan;
using agentd::voice_peer_broker_session_plan_json;

static void test_borrowed_broker_session_plan() {
  VoicePeerStartPlan start_plan;
  start_plan.requested_broker_session_id = "sess-1";
  start_plan.requested_broker_session_preflighted = true;
  start_plan.requested_broker_session_mode = "webrtc";

  const agentd::VoicePeerBrokerSessionPlan plan =
    make_voice_peer_broker_session_plan(start_plan);
  assert(plan.mode == "borrowed");
  assert(plan.session_id == "sess-1");
  assert(plan.preflighted);
  assert(plan.session_mode == "webrtc");
  assert(!plan.managed_broker_session);
  assert(plan.agent_id.empty());
  assert(plan.deployment_id.empty());

  const Json::Value out = voice_peer_broker_session_plan_json(plan);
  assert(out["mode"].asString() == "borrowed");
  assert(out["session_id"].asString() == "sess-1");
  assert(out["preflighted"].asBool());
  assert(out["session_mode"].asString() == "webrtc");
  assert(out["agent_id"].isNull());
  assert(out["deployment_id"].isNull());
}

static void test_auto_create_broker_session_plan() {
  VoicePeerStartPlan start_plan;
  start_plan.broker_agent_id = "agent-a";
  start_plan.broker_deployment_id = "deploy-b";

  const agentd::VoicePeerBrokerSessionPlan plan =
    make_voice_peer_broker_session_plan(start_plan);
  assert(plan.mode == "auto_create");
  assert(plan.session_id.empty());
  assert(!plan.preflighted);
  assert(plan.session_mode.empty());
  assert(plan.agent_id == "agent-a");
  assert(plan.deployment_id == "deploy-b");
  assert(plan.managed_broker_session);

  const Json::Value out = voice_peer_broker_session_plan_json(plan);
  assert(out["mode"].asString() == "auto_create");
  assert(!out["preflighted"].asBool());
  assert(out["session_id"].isNull());
  assert(out["session_mode"].isNull());
  assert(out["agent_id"].asString() == "agent-a");
  assert(out["deployment_id"].asString() == "deploy-b");
}

}  // namespace

int main() {
  test_borrowed_broker_session_plan();
  test_auto_create_broker_session_plan();
  return 0;
}
