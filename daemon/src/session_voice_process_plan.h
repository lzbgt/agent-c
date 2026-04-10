#pragma once

#include "session_voice_broker_plan.h"
#include "session_voice_broker_session.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_start_plan.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace agentd {

struct VoicePeerMediaRuntimePlan {
  std::string schema = "voice_webrtc_peer_media_runtime_plan_v1";
  std::string signaling_surface = "voice_webrtc_peer";
  std::string runtime_kind = "external";
  std::string media_engine_kind = "browser_peer";
  std::string session_id;
  std::string broker_session_id;
  std::string broker_url;
  bool managed_broker_session = false;
  std::string broker_agent_id;
  std::string broker_deployment_id;
  std::string sender_tag;
  std::string ready_file_path;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  bool native_media_supported = false;
  Json::Value native_media_provider = Json::Value(Json::nullValue);
};

struct VoicePeerChildProcessPlan {
  VoicePeerMediaRuntimePlan media_runtime_plan;
  std::vector<std::string> argv;
};

VoicePeerMediaRuntimePlan make_voice_peer_media_runtime_plan(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
  const VoicePeerBrokerSessionPlan& broker_session_plan
);

Json::Value voice_peer_media_runtime_plan_json(
  const VoicePeerMediaRuntimePlan& plan
);

VoicePeerChildProcessPlan make_voice_peer_child_process_plan(
  const VoicePeerChildLaunchConfig& launch_cfg,
  const VoicePeerRuntimeArtifactsPlan& artifacts
);

VoicePeerChildLaunchConfig make_voice_peer_child_launch_config(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerBrokerSessionBinding& binding
);

}  // namespace agentd
