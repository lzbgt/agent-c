#include "session_voice_builtin_contract.h"

#include <cassert>
#include <string>

namespace {

using agentd::VoicePeerStartPlan;
using agentd::session_voice_builtin_start_contract_json;

static void test_borrowed_broker_session_contract() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.requested_broker_session_id = "sess-1";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1111;
  plan.poll_interval_ms = 222;
  plan.tone_hz = 333;
  plan.startup_wait_ms = 444;

  const Json::Value out = session_voice_builtin_start_contract_json("voice-sid", plan);
  assert(out["session_id"].asString() == "voice-sid");
  assert(out["runtime_kind"].asString() == "builtin");
  assert(out["signaling_surface"].asString() == "voice_webrtc_peer");
  assert(out["broker_url"].asString() == "http://broker");
  assert(out["sender_tag"].asString() == "agentd_runtime_peer");
  assert(out["deadline_ms"].asInt64() == 1111);
  assert(out["poll_interval_ms"].asInt64() == 222);
  assert(out["tone_hz"].asInt64() == 333);
  assert(out["startup_wait_ms"].asInt64() == 444);
  assert(out["mutating_broker_actions_deferred"].asBool());
  assert(out["broker_session"]["mode"].asString() == "borrowed");
  assert(out["broker_session"]["session_id"].asString() == "sess-1");
  assert(out["broker_session"]["preflighted"].asBool());
  assert(out["broker_session"]["session_mode"].asString() == "webrtc");
}

static void test_auto_create_broker_session_contract() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";

  const Json::Value out = session_voice_builtin_start_contract_json("voice-sid", plan);
  assert(out["broker_session"]["mode"].asString() == "auto_create");
  assert(!out["broker_session"]["preflighted"].asBool());
  assert(out["broker_session"]["agent_id"].asString() == "agent-a");
  assert(out["broker_session"]["deployment_id"].asString() == "deploy-b");
}

}  // namespace

int main() {
  test_borrowed_broker_session_contract();
  test_auto_create_broker_session_contract();
  return 0;
}
