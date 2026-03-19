#include "session_voice_builtin_backend.h"

#include "session_voice_backend_policy.h"
#include "session_voice_broker_plan.h"
#include "session_voice_builtin_contract.h"
#include "session_voice_runtime_plan.h"
#include "string_util.h"

#include <memory>

namespace agentd {

bool start_voice_peer_builtin_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  VoicePeerBackendStartResult* out_result
) {
  if (!out_result) return false;
  *out_result = VoicePeerBackendStartResult{};
  out_result->http_status = 501;
  out_result->error = voice_peer_backend_unavailable_reason(cfg, start_plan.runtime_kind);
  if (trim_copy(out_result->error).empty()) {
    out_result->error = "builtin voice_webrtc_peer runtime not implemented";
  }
  const VoicePeerBuiltinStartPreview preview =
    build_voice_peer_builtin_start_preview(cfg, session_id, start_plan);
  out_result->state = std::make_shared<VoicePeerRuntime>(preview.planned_runtime);
  out_result->backend_info["builtin_start_contract"] = preview.contract;
  return false;
}

}  // namespace agentd
