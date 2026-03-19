#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {

struct VoicePeerPlanError {
  int http_status = 0;
  bool use_json_error_body = false;
  std::string message;
};

struct VoicePeerStartPlan {
  std::string runtime_kind;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  int64_t startup_wait_ms = 2000;
  std::string request_broker_url;
  std::string requested_broker_session_id;
  std::string broker_agent_id;
  std::string broker_deployment_id;
  std::string sender_tag = "agentd_runtime_peer";
  std::string effective_broker_url;
  std::string desired_tool_path;
  std::string desired_node_bin;
  std::string resolved_tool_path;
  std::string resolved_node_bin;
  std::string broker_token;
  std::string requested_broker_session_mode;
  bool desired_backend_available = false;
  bool requested_broker_session_preflighted = false;
  std::string desired_backend_err;
  bool has_requested_broker_session_id = false;
  bool has_broker_agent_id = false;
  bool has_broker_deployment_id = false;
  bool has_sender_tag = false;
  bool has_deadline_ms = false;
  bool has_poll_interval_ms = false;
  bool has_tone_hz = false;
};

bool build_voice_peer_start_plan(
  const DaemonConfig& cfg,
  const Json::Value& body,
  VoicePeerStartPlan* out_plan,
  VoicePeerPlanError* out_err
);

bool finalize_voice_peer_start_plan_for_launch(
  const DaemonConfig& cfg,
  const std::string& request_broker_token,
  VoicePeerStartPlan* plan,
  VoicePeerPlanError* out_err
);

}  // namespace agentd
