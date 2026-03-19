#include "session_voice_broker_session.h"

#include "session_voice_broker_client.h"

namespace agentd {

bool resolve_voice_peer_broker_session(
  const VoicePeerStartPlan& start_plan,
  VoicePeerBrokerSessionBinding* out_binding,
  int* out_http_status,
  std::string* out_err
) {
  if (out_http_status) *out_http_status = 500;
  if (out_err) out_err->clear();
  if (!out_binding) return false;
  *out_binding = VoicePeerBrokerSessionBinding{};

  out_binding->broker_session_id = start_plan.requested_broker_session_id;
  if (out_binding->broker_session_id.empty()) {
    if (!broker_create_audio_session(
          start_plan.effective_broker_url,
          start_plan.broker_token,
          start_plan.broker_agent_id,
          start_plan.broker_deployment_id,
          &out_binding->broker_session_id,
          out_err)) {
      if (out_http_status) *out_http_status = 500;
      if (out_err && out_err->empty()) *out_err = "failed to create broker audio session";
      return false;
    }
    out_binding->managed_broker_session = true;
    return true;
  }

  if (start_plan.requested_broker_session_preflighted) {
    return true;
  }

  bool session_exists = false;
  std::string broker_session_mode;
  if (!broker_audio_session_exists(
        start_plan.effective_broker_url,
        start_plan.broker_token,
        out_binding->broker_session_id,
        &session_exists,
        &broker_session_mode,
        out_err)) {
    if (out_http_status) *out_http_status = 500;
    if (out_err && out_err->empty()) *out_err = "failed to inspect broker audio session";
    return false;
  }
  if (!session_exists) {
    if (out_http_status) *out_http_status = 400;
    if (out_err) *out_err = "broker_session_id not found";
    return false;
  }
  if (!broker_session_mode.empty() && broker_session_mode != "webrtc") {
    if (out_http_status) *out_http_status = 400;
    if (out_err) *out_err = "broker_session_id mode must be webrtc";
    return false;
  }
  return true;
}

bool release_managed_voice_peer_broker_session(
  const VoicePeerStartPlan& start_plan,
  const VoicePeerBrokerSessionBinding& binding,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!binding.managed_broker_session || binding.broker_session_id.empty()) return true;
  if (broker_delete_audio_session(
        start_plan.effective_broker_url,
        start_plan.broker_token,
        binding.broker_session_id,
        out_err)) {
    return true;
  }
  if (out_err && out_err->empty()) *out_err = "failed to delete managed broker audio session";
  return false;
}

}  // namespace agentd
