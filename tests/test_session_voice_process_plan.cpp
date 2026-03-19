#include "session_voice_process_plan.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::VoicePeerChildLaunchConfig;
using agentd::VoicePeerChildProcessPlan;
using agentd::VoicePeerRuntimeArtifactsPlan;
using agentd::VoicePeerStartPlan;
using agentd::make_voice_peer_child_process_plan;
using agentd::make_voice_peer_media_runtime_plan;
using agentd::voice_peer_media_runtime_plan_json;

static VoicePeerRuntimeArtifactsPlan make_artifacts() {
  VoicePeerRuntimeArtifactsPlan artifacts;
  artifacts.runtime_dir = "/tmp/agentd-state/voice_webrtc_peers/voice-sid";
  artifacts.ready_file_path = artifacts.runtime_dir + "/ready.json";
  artifacts.stdout_log_path = artifacts.runtime_dir + "/stdout.jsonl";
  artifacts.stderr_log_path = artifacts.runtime_dir + "/stderr.log";
  return artifacts;
}

static void test_media_runtime_plan_carries_safe_runtime_inputs() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1234;
  plan.poll_interval_ms = 234;
  plan.tone_hz = 345;

  const VoicePeerRuntimeArtifactsPlan artifacts = make_artifacts();
  const agentd::VoicePeerMediaRuntimePlan media_plan =
    make_voice_peer_media_runtime_plan("voice-sid", plan, artifacts, "", true);

  assert(media_plan.schema == "voice_webrtc_peer_media_runtime_plan_v1");
  assert(media_plan.signaling_surface == "voice_webrtc_peer");
  assert(media_plan.runtime_kind == "builtin");
  assert(media_plan.session_id == "voice-sid");
  assert(media_plan.broker_session_id.empty());
  assert(media_plan.broker_url == "http://broker");
  assert(media_plan.managed_broker_session);
  assert(media_plan.broker_agent_id == "agent-a");
  assert(media_plan.broker_deployment_id == "deploy-b");
  assert(media_plan.sender_tag == "agentd_runtime_peer");
  assert(media_plan.ready_file_path == artifacts.ready_file_path);
  assert(media_plan.deadline_ms == 1234);
  assert(media_plan.poll_interval_ms == 234);
  assert(media_plan.tone_hz == 345);

  const Json::Value out = voice_peer_media_runtime_plan_json(media_plan);
  assert(out["schema"].asString() == media_plan.schema);
  assert(out["signaling_surface"].asString() == "voice_webrtc_peer");
  assert(out["runtime_kind"].asString() == "builtin");
  assert(out["session_id"].asString() == "voice-sid");
  assert(out["broker_session_id"].isNull());
  assert(out["broker_url"].asString() == "http://broker");
  assert(out["managed_broker_session"].asBool());
  assert(out["broker_agent_id"].asString() == "agent-a");
  assert(out["broker_deployment_id"].asString() == "deploy-b");
  assert(out["sender_tag"].asString() == "agentd_runtime_peer");
  assert(out["ready_signal"].asString() == "ready_file");
  assert(out["ready_file_path"].asString() == artifacts.ready_file_path);
  assert(out["deadline_ms"].asInt64() == 1234);
  assert(out["poll_interval_ms"].asInt64() == 234);
  assert(out["tone_hz"].asInt64() == 345);
}

static void test_child_process_plan_shapes_media_plan_and_argv() {
  VoicePeerChildLaunchConfig launch_cfg;
  launch_cfg.runtime_kind = "bundled";
  launch_cfg.session_id = "voice-sid";
  launch_cfg.broker_session_id = "sess-1";
  launch_cfg.broker_url = "http://broker";
  launch_cfg.broker_token = "secret-token";
  launch_cfg.broker_agent_id = "agent-a";
  launch_cfg.broker_deployment_id = "deploy-b";
  launch_cfg.sender_tag = "agentd_runtime_peer";
  launch_cfg.tool_path = "/tool.js";
  launch_cfg.node_bin = "/usr/bin/node";
  launch_cfg.deadline_ms = 1111;
  launch_cfg.poll_interval_ms = 222;
  launch_cfg.tone_hz = 333;
  launch_cfg.managed_broker_session = true;

  const VoicePeerRuntimeArtifactsPlan artifacts = make_artifacts();
  const VoicePeerChildProcessPlan plan =
    make_voice_peer_child_process_plan(launch_cfg, artifacts);

  assert(plan.media_runtime_plan.schema == "voice_webrtc_peer_media_runtime_plan_v1");
  assert(plan.media_runtime_plan.runtime_kind == "bundled");
  assert(plan.media_runtime_plan.session_id == "voice-sid");
  assert(plan.media_runtime_plan.broker_session_id == "sess-1");
  assert(plan.media_runtime_plan.broker_url == "http://broker");
  assert(plan.media_runtime_plan.managed_broker_session);
  assert(plan.media_runtime_plan.broker_agent_id == "agent-a");
  assert(plan.media_runtime_plan.broker_deployment_id == "deploy-b");
  assert(plan.media_runtime_plan.sender_tag == "agentd_runtime_peer");
  assert(plan.media_runtime_plan.ready_file_path == artifacts.ready_file_path);
  assert(plan.media_runtime_plan.deadline_ms == 1111);
  assert(plan.media_runtime_plan.poll_interval_ms == 222);
  assert(plan.media_runtime_plan.tone_hz == 333);

  const std::vector<std::string> expected = {
    "/usr/bin/node",
    "/tool.js",
    "--broker-url",
    "http://broker",
    "--token",
    "secret-token",
    "--session-id",
    "sess-1",
    "--ready-file",
    artifacts.ready_file_path,
    "--deadline-ms",
    "1111",
    "--poll-interval-ms",
    "222",
    "--tone-hz",
    "333",
    "--sender-tag",
    "agentd_runtime_peer",
  };
  assert(plan.argv == expected);
}

}  // namespace

int main() {
  test_media_runtime_plan_carries_safe_runtime_inputs();
  test_child_process_plan_shapes_media_plan_and_argv();
  return 0;
}
