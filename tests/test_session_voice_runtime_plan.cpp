#include "session_voice_runtime_plan.h"

#include <cassert>
#include <string>

namespace {

using agentd::DaemonConfig;
using agentd::VoicePeerChildLaunchConfig;
using agentd::VoicePeerRuntimeArtifactsPlan;
using agentd::VoicePeerRuntimeSeed;
using agentd::VoicePeerStartPlan;
using agentd::make_planned_voice_peer_runtime;
using agentd::make_spawned_voice_peer_runtime_seed;
using agentd::plan_voice_peer_runtime_artifacts;
using agentd::voice_peer_runtime_artifacts_json;

static void test_runtime_artifacts_follow_state_dir() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";

  const VoicePeerRuntimeArtifactsPlan plan =
    plan_voice_peer_runtime_artifacts(cfg, "voice-sid");
  assert(plan.runtime_dir == "/tmp/agentd-state/voice_webrtc_peers/voice-sid");
  assert(plan.ready_file_path == plan.runtime_dir + "/ready.json");
  assert(plan.stdout_log_path == plan.runtime_dir + "/stdout.jsonl");
  assert(plan.stderr_log_path == plan.runtime_dir + "/stderr.log");

  const Json::Value out = voice_peer_runtime_artifacts_json(plan);
  assert(out["runtime_dir"].asString() == plan.runtime_dir);
  assert(out["ready_file_path"].asString() == plan.ready_file_path);
  assert(out["stdout_log_path"].asString() == plan.stdout_log_path);
  assert(out["stderr_log_path"].asString() == plan.stderr_log_path);
  assert(out["ready_signal"].asString() == "ready_file");
  assert(out["stdout_format"].asString() == "jsonl");
  assert(out["stderr_format"].asString() == "text");
}

static void test_spawned_runtime_seed_carries_launch_and_artifacts() {
  VoicePeerChildLaunchConfig launch_cfg;
  launch_cfg.runtime_kind = "external";
  launch_cfg.session_id = "voice-sid";
  launch_cfg.broker_session_id = "broker-sess";
  launch_cfg.broker_url = "http://broker";
  launch_cfg.broker_agent_id = "agent-a";
  launch_cfg.broker_deployment_id = "deploy-b";
  launch_cfg.sender_tag = "agentd_runtime_peer";
  launch_cfg.tool_path = "/tool.js";
  launch_cfg.node_bin = "/usr/bin/node";
  launch_cfg.deadline_ms = 1111;
  launch_cfg.poll_interval_ms = 222;
  launch_cfg.tone_hz = 333;
  launch_cfg.managed_broker_session = true;

  VoicePeerRuntimeArtifactsPlan plan;
  plan.runtime_dir = "/tmp/agentd-state/voice_webrtc_peers/voice-sid";
  plan.ready_file_path = plan.runtime_dir + "/ready.json";
  plan.stdout_log_path = plan.runtime_dir + "/stdout.jsonl";
  plan.stderr_log_path = plan.runtime_dir + "/stderr.log";

#if defined(_WIN32)
  const VoicePeerRuntimeSeed seed =
    make_spawned_voice_peer_runtime_seed(launch_cfg, plan, 123);
#else
  const VoicePeerRuntimeSeed seed =
    make_spawned_voice_peer_runtime_seed(launch_cfg, plan, 456);
#endif

  assert(seed.runtime_kind == "external");
  assert(seed.session_id == "voice-sid");
  assert(seed.broker_session_id == "broker-sess");
  assert(seed.broker_url == "http://broker");
  assert(seed.broker_agent_id == "agent-a");
  assert(seed.broker_deployment_id == "deploy-b");
  assert(seed.sender_tag == "agentd_runtime_peer");
  assert(seed.tool_path == "/tool.js");
  assert(seed.node_bin == "/usr/bin/node");
  assert(seed.ready_file_path == plan.ready_file_path);
  assert(seed.stdout_log_path == plan.stdout_log_path);
  assert(seed.stderr_log_path == plan.stderr_log_path);
  assert(seed.deadline_ms == 1111);
  assert(seed.poll_interval_ms == 222);
  assert(seed.tone_hz == 333);
  assert(seed.managed_broker_session);
  assert(!seed.ready);
  assert(seed.running);
#if defined(_WIN32)
  assert(seed.pid == 123);
#else
  assert(seed.pid == 456);
#endif
}

static void test_planned_runtime_matches_start_plan_and_artifacts() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1234;
  plan.poll_interval_ms = 234;
  plan.tone_hz = 345;

  VoicePeerRuntimeArtifactsPlan artifacts;
  artifacts.runtime_dir = "/tmp/agentd-state/voice_webrtc_peers/voice-sid";
  artifacts.ready_file_path = artifacts.runtime_dir + "/ready.json";
  artifacts.stdout_log_path = artifacts.runtime_dir + "/stdout.jsonl";
  artifacts.stderr_log_path = artifacts.runtime_dir + "/stderr.log";

  const agentd::VoicePeerRuntime runtime =
    make_planned_voice_peer_runtime("voice-sid", plan, artifacts, "", true);
  assert(runtime.runtime_kind == "builtin");
  assert(runtime.session_id == "voice-sid");
  assert(runtime.broker_session_id.empty());
  assert(runtime.broker_url == "http://broker");
  assert(runtime.managed_broker_session);
  assert(runtime.broker_agent_id == "agent-a");
  assert(runtime.broker_deployment_id == "deploy-b");
  assert(runtime.sender_tag == "agentd_runtime_peer");
  assert(runtime.ready_file_path == artifacts.ready_file_path);
  assert(runtime.stdout_log_path == artifacts.stdout_log_path);
  assert(runtime.stderr_log_path == artifacts.stderr_log_path);
  assert(runtime.deadline_ms == 1234);
  assert(runtime.poll_interval_ms == 234);
  assert(runtime.tone_hz == 345);
  assert(!runtime.ready);
  assert(!runtime.running);
}

}  // namespace

int main() {
  test_runtime_artifacts_follow_state_dir();
  test_spawned_runtime_seed_carries_launch_and_artifacts();
  test_planned_runtime_matches_start_plan_and_artifacts();
  return 0;
}
