#include "session_voice_builtin_media_engine.h"

#include "session_voice_backend_policy.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_internal.h"
#include "string_util.h"

#include <chrono>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json::Value make_engine_event(
  const VoicePeerMediaEngineInfo& info,
  const std::string& event,
  const std::string& state
) {
  Json::Value payload(Json::objectValue);
  payload["ok"] = true;
  payload["event"] = event;
  payload["media_engine_kind"] = info.media_engine_kind;
  payload["media_engine_state"] = state;
  payload["native_media_supported"] = info.native_media_supported;
  payload["native_media_active"] = info.native_media_active;
  return payload;
}

class BuiltinVoicePeerSignalingStubEngine final : public VoicePeerBuiltinMediaEngine {
 public:
  VoicePeerMediaEngineInfo info() const override {
    VoicePeerMediaEngineInfo out;
    out.media_engine_kind = "builtin_signaling_stub";
    out.native_media_supported = false;
    out.native_media_active = false;
    return out;
  }

  bool initialize(
    VoicePeerRuntime* runtime,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (runtime) apply_voice_peer_media_engine_info(info(), runtime);
    if (out_event) *out_event = make_engine_event(info(), "media_engine_initialized", "starting");
    return true;
  }

  bool handle_remote_description(
    const VoiceBrokerSignalRemoteDescriptionReady& ready,
    VoiceBrokerSignalDescription* out_answer,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    const std::string desc_type = lower_copy(trim_copy(ready.description.type));
    if (!desc_type.empty() && desc_type != "offer") {
      if (out_err) *out_err = "expected remote offer, got " + desc_type;
      return false;
    }

    if (out_answer) {
      out_answer->type = "answer";
      out_answer->sdp = "stub-answer";
    }
    if (out_event) {
      Json::Value payload = make_engine_event(info(), "stub_answer_ready", "answer_ready");
      payload["initial_remote_candidate_count"] = Json::UInt64(ready.initial_remote_candidates.size());
      *out_event = payload;
    }
    return true;
  }

  bool handle_remote_candidate(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (out_event) {
      Json::Value payload = make_engine_event(info(), "remote_candidate_ready", "signaling_active");
      payload["candidate"] = ingress.candidate.candidate;
      if (!trim_copy(ingress.candidate.sdp_mid).empty()) payload["sdpMid"] = ingress.candidate.sdp_mid;
      if (ingress.candidate.has_sdp_mline_index) payload["sdpMLineIndex"] = ingress.candidate.sdp_mline_index;
      *out_event = payload;
    }
    return true;
  }

  void handle_remote_bye(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event
  ) override {
    if (!out_event) return;
    Json::Value payload = make_engine_event(info(), "remote_bye", "stopped");
    if (!trim_copy(ingress.bye.reason).empty()) payload["reason"] = ingress.bye.reason;
    *out_event = payload;
  }

  void handle_local_shutdown(
    Json::Value* out_event
  ) override {
    if (!out_event) return;
    Json::Value payload = make_engine_event(info(), "local_bye_sent", "stopping");
    payload["reason"] = "agentd_builtin_stop";
    *out_event = payload;
  }
};

}  // namespace

VoicePeerMediaEngineInfo voice_peer_media_engine_info_for_runtime_kind(
  const DaemonConfig& cfg,
  const std::string& runtime_kind
) {
  VoicePeerMediaEngineInfo out;
  const std::string kind = lower_copy(trim_copy(runtime_kind));
  if (kind == "bundled" || kind == "external") {
    out.media_engine_kind = "browser_peer";
    return out;
  }
  if (kind == "builtin") {
    if (voice_peer_builtin_runtime_mode(cfg) == "signaling_stub") {
      out.media_engine_kind = "builtin_signaling_stub";
      return out;
    }
    out.media_engine_kind = "builtin_reserved";
    return out;
  }
  out.media_engine_kind = "unknown";
  return out;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerMediaRuntimePlan* plan
) {
  if (!plan) return;
  plan->media_engine_kind = info.media_engine_kind;
  plan->native_media_supported = info.native_media_supported;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntimeSeed* seed
) {
  if (!seed) return;
  seed->media_engine_kind = info.media_engine_kind;
  seed->native_media_supported = info.native_media_supported;
  seed->native_media_active = info.native_media_active;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntime* runtime
) {
  if (!runtime) return;
  runtime->media_engine_kind = info.media_engine_kind;
  runtime->native_media_supported = info.native_media_supported;
  runtime->native_media_active = info.native_media_active;
}

void set_voice_peer_media_engine_state(
  VoicePeerRuntimeSeed* seed,
  const std::string& state,
  int64_t ts_unix_ms
) {
  if (!seed) return;
  seed->media_engine_state = trim_copy(state).empty() ? "idle" : trim_copy(state);
  seed->media_state_updated_unix_ms = ts_unix_ms;
}

void set_voice_peer_media_engine_state(
  VoicePeerRuntime* runtime,
  const std::string& state,
  int64_t ts_unix_ms
) {
  if (!runtime) return;
  runtime->media_engine_state = trim_copy(state).empty() ? "idle" : trim_copy(state);
  runtime->media_state_updated_unix_ms = ts_unix_ms > 0 ? ts_unix_ms : now_unix_ms();
}

void note_voice_peer_media_engine_event(
  VoicePeerRuntime* runtime,
  const Json::Value& payload
) {
  if (!runtime || !payload.isObject()) return;
  const std::string event =
    payload.isMember("event") && payload["event"].isString()
      ? lower_copy(trim_copy(payload["event"].asString()))
      : std::string();
  const std::string explicit_state =
    payload.isMember("media_engine_state") && payload["media_engine_state"].isString()
      ? trim_copy(payload["media_engine_state"].asString())
      : std::string();
  const int64_t ts =
    payload.isMember("ts_unix_ms") &&
        (payload["ts_unix_ms"].isInt64() || payload["ts_unix_ms"].isUInt64())
      ? payload["ts_unix_ms"].asInt64()
      : now_unix_ms();

  runtime->media_events_total += 1;
  if (event == "remote_offer_seen") runtime->media_remote_offers_seen += 1;
  if (event == "stub_answer_sent") runtime->media_answers_sent += 1;
  if (event == "remote_candidate_ready") runtime->media_remote_candidates_seen += 1;
  if (event == "remote_bye") runtime->media_remote_byes_seen += 1;
  if (event == "local_bye_sent") runtime->media_local_byes_sent += 1;

  if (!explicit_state.empty()) {
    runtime->media_engine_state = explicit_state;
    runtime->media_state_updated_unix_ms = ts;
    return;
  }

  if (event == "builtin_runtime_started") {
    runtime->media_engine_state = "signaling_ready";
    runtime->media_state_updated_unix_ms = ts;
  } else if (event == "builtin_runtime_stopped") {
    runtime->media_engine_state = "stopped";
    runtime->media_state_updated_unix_ms = ts;
  } else if (event == "builtin_runtime_failed") {
    runtime->media_engine_state = "failed";
    runtime->media_state_updated_unix_ms = ts;
  }
}

std::unique_ptr<VoicePeerBuiltinMediaEngine> make_builtin_voice_peer_media_engine(
  const DaemonConfig& cfg,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (voice_peer_builtin_runtime_mode(cfg) == "signaling_stub") {
    return std::make_unique<BuiltinVoicePeerSignalingStubEngine>();
  }
  if (out_err) *out_err = "builtin voice_webrtc_peer runtime disabled";
  return nullptr;
}

}  // namespace agentd
