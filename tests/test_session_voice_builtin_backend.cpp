#include "session_voice_builtin_backend.h"

#include <cassert>
#include <string>

namespace {

using agentd::DaemonConfig;
using agentd::VoicePeerBackendStartResult;
using agentd::VoicePeerStartPlan;
using agentd::start_voice_peer_builtin_backend;

static VoicePeerStartPlan make_plan() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1111;
  plan.poll_interval_ms = 222;
  plan.tone_hz = 333;
  return plan;
}

static void test_builtin_backend_returns_planned_runtime_state() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerBackendStartResult result;

  assert(!start_voice_peer_builtin_backend(cfg, "voice-sid", make_plan(), &result));
  assert(result.http_status == 501);
  assert(result.state);
  assert(result.state->runtime_kind == "builtin");
  assert(result.state->status_source == "planned");
  assert(result.state->session_id == "voice-sid");
  assert(result.state->broker_url == "http://broker");
  assert(result.state->managed_broker_session);
  assert(result.state->broker_agent_id == "agent-a");
  assert(result.state->broker_deployment_id == "deploy-b");
  assert(result.state->tool_path == "@builtin");
  assert(result.state->node_bin == "@builtin");
  assert(result.state->ready_file_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/ready.json");
  assert(result.state->stdout_log_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stdout.jsonl");
  assert(result.state->stderr_log_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stderr.log");
  assert(!result.state->running);
  assert(!result.state->ready);
  assert(result.state->last_error == result.error);
  assert(result.backend_info["builtin_start_contract"].isObject());
}

static void test_builtin_backend_borrowed_session_preview_is_not_managed() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerStartPlan plan = make_plan();
  plan.requested_broker_session_id = "sess-1";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.broker_agent_id.clear();
  plan.broker_deployment_id.clear();

  VoicePeerBackendStartResult result;
  assert(!start_voice_peer_builtin_backend(cfg, "voice-sid", plan, &result));
  assert(result.state);
  assert(result.state->status_source == "planned");
  assert(!result.state->managed_broker_session);
  assert(result.state->broker_session_id == "sess-1");
}

}  // namespace

int main() {
  test_builtin_backend_returns_planned_runtime_state();
  test_builtin_backend_borrowed_session_preview_is_not_managed();
  return 0;
}
