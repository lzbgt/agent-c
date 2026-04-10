#include "session_voice_builtin_media_engine.h"

#include "session_voice_runtime_internal.h"

#include <cassert>
#include <string>

namespace {

using agentd::DaemonConfig;
using agentd::VoiceBrokerSignalIngress;
using agentd::VoiceBrokerSignalIngressKind;
using agentd::VoiceBrokerSignalRemoteDescriptionReady;
using agentd::VoicePeerRuntime;
using agentd::make_builtin_voice_peer_media_engine;
using agentd::voice_peer_media_engine_info_for_runtime_kind;

static void test_runtime_kind_info_reports_reserved_and_stub_modes() {
  DaemonConfig disabled_cfg;
  const auto disabled_info =
    voice_peer_media_engine_info_for_runtime_kind(disabled_cfg, "builtin");
  assert(disabled_info.media_engine_kind == "builtin_reserved");
  assert(!disabled_info.native_media_supported);
  assert(!disabled_info.native_media_active);

  DaemonConfig enabled_cfg;
  enabled_cfg.audio_webrtc_builtin_mode = "signaling_stub";
  const auto enabled_info =
    voice_peer_media_engine_info_for_runtime_kind(enabled_cfg, "builtin");
  assert(enabled_info.media_engine_kind == "builtin_signaling_stub");
  assert(!enabled_info.native_media_supported);
  assert(!enabled_info.native_media_active);

  const auto child_info =
    voice_peer_media_engine_info_for_runtime_kind(enabled_cfg, "bundled");
  assert(child_info.media_engine_kind == "browser_peer");
  assert(!child_info.native_media_supported);
}

static void test_builtin_media_engine_stub_answers_remote_offer() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "signaling_stub";

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().media_engine_kind == "builtin_signaling_stub");

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  assert(runtime.media_engine_kind == "builtin_signaling_stub");
  assert(init_event["event"].asString() == "media_engine_initialized");

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = "remote-offer";
  ready.initial_remote_candidates.resize(2);

  agentd::VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  assert(answer.type == "answer");
  assert(answer.sdp == "stub-answer");
  assert(answer_event["event"].asString() == "stub_answer_ready");
  assert(answer_event["initial_remote_candidate_count"].asUInt() == 2u);

  VoiceBrokerSignalIngress candidate;
  candidate.kind = VoiceBrokerSignalIngressKind::remote_candidate_ready;
  candidate.candidate.candidate = "candidate:1";
  candidate.candidate.sdp_mid = "audio";
  candidate.candidate.sdp_mline_index = 0;
  candidate.candidate.has_sdp_mline_index = true;

  Json::Value candidate_event(Json::nullValue);
  assert(engine->handle_remote_candidate(candidate, &candidate_event, &err));
  assert(err.empty());
  assert(candidate_event["event"].asString() == "remote_candidate_ready");
  assert(candidate_event["candidate"].asString() == "candidate:1");
  assert(candidate_event["sdpMid"].asString() == "audio");
  assert(candidate_event["sdpMLineIndex"].asInt() == 0);

  VoiceBrokerSignalIngress bye;
  bye.kind = VoiceBrokerSignalIngressKind::remote_bye;
  bye.bye.reason = "remote_done";
  Json::Value bye_event(Json::nullValue);
  engine->handle_remote_bye(bye, &bye_event);
  assert(bye_event["event"].asString() == "remote_bye");
  assert(bye_event["reason"].asString() == "remote_done");

  Json::Value shutdown_event(Json::nullValue);
  engine->handle_local_shutdown(&shutdown_event);
  assert(shutdown_event["event"].asString() == "local_bye_sent");
  assert(shutdown_event["reason"].asString() == "agentd_builtin_stop");
}

}  // namespace

int main() {
  test_runtime_kind_info_reports_reserved_and_stub_modes();
  test_builtin_media_engine_stub_answers_remote_offer();
  return 0;
}
