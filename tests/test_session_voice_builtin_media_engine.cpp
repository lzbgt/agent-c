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
using agentd::note_voice_peer_media_engine_event;
using agentd::voice_peer_media_engine_info_for_runtime_kind;

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH ""
#endif

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH ""
#endif

static void assert_known_test_provider(
  const std::string& provider_name,
  const Json::Value& capabilities
) {
  if (provider_name == "agentd_builtin_sample_provider") {
    assert(capabilities["transport_family"].asString() == "sample_webrtc");
    assert(capabilities["sample_provider"].asBool());
    assert(capabilities["real_media_engine"].asBool() == false);
    return;
  }
  if (provider_name == "agentd_builtin_embedded_transport_provider") {
    assert(capabilities["transport_family"].asString() == "embedded_transport_primitives");
    assert(capabilities["embedded_transport_provider"].asBool());
    assert(capabilities["ice"].asBool());
    assert(capabilities["dtls"].asBool());
    assert(capabilities["dtls_handshake"].asBool());
    assert(capabilities["dtls_srtp_export"].asBool());
    assert(capabilities["srtp_contexts"].asBool());
    assert(capabilities["srtp"].asBool());
    assert(capabilities["sctp"].asBool());
    assert(capabilities["real_media_engine"].asBool() == false);
    return;
  }
  assert(false && "unexpected builtin native provider");
}

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

  DaemonConfig native_cfg;
  native_cfg.audio_webrtc_builtin_mode = "native_plugin";
  native_cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;
  const auto native_info =
    voice_peer_media_engine_info_for_runtime_kind(native_cfg, "builtin");
  assert(native_info.media_engine_kind == "builtin_native_plugin");
  assert(!native_info.native_media_supported);
  assert(!native_info.native_media_active);
  assert(native_info.provider_abi_version == 3);
  assert(!native_info.provider_name.empty());
  assert(!native_info.provider_version.empty());
  assert_known_test_provider(native_info.provider_name, native_info.provider_capabilities);
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
  assert(init_event["media_engine_state"].asString() == "signaling_ready");
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(runtime.media_engine_state == "signaling_ready");
  assert(runtime.media_events_total == 1);

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
  assert(answer_event["media_engine_state"].asString() == "answer_ready");
  assert(answer_event["initial_remote_candidate_count"].asUInt() == 2u);
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(runtime.media_engine_state == "answer_ready");
  assert(runtime.media_events_total == 2);

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
  assert(candidate_event["media_engine_state"].asString() == "signaling_active");
  assert(candidate_event["candidate"].asString() == "candidate:1");
  assert(candidate_event["sdpMid"].asString() == "audio");
  assert(candidate_event["sdpMLineIndex"].asInt() == 0);
  note_voice_peer_media_engine_event(&runtime, candidate_event);
  assert(runtime.media_engine_state == "signaling_active");
  assert(runtime.media_remote_candidates_seen == 1);

  VoiceBrokerSignalIngress bye;
  bye.kind = VoiceBrokerSignalIngressKind::remote_bye;
  bye.bye.reason = "remote_done";
  Json::Value bye_event(Json::nullValue);
  engine->handle_remote_bye(bye, &bye_event);
  assert(bye_event["event"].asString() == "remote_bye");
  assert(bye_event["media_engine_state"].asString() == "stopped");
  assert(bye_event["reason"].asString() == "remote_done");
  note_voice_peer_media_engine_event(&runtime, bye_event);
  assert(runtime.media_engine_state == "stopped");
  assert(runtime.media_remote_byes_seen == 1);

  Json::Value shutdown_event(Json::nullValue);
  engine->handle_local_shutdown(&shutdown_event);
  assert(shutdown_event["event"].asString() == "local_bye_sent");
  assert(shutdown_event["media_engine_state"].asString() == "stopping");
  assert(shutdown_event["reason"].asString() == "agentd_builtin_stop");
  note_voice_peer_media_engine_event(&runtime, shutdown_event);
  assert(runtime.media_engine_state == "stopping");
  assert(runtime.media_local_byes_sent == 1);
}

static void test_builtin_media_engine_native_plugin_loads_sample_provider_metadata() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().media_engine_kind == "builtin_native_plugin");
  assert(!engine->info().native_media_supported);
  assert(!engine->info().native_media_active);
  assert(engine->info().provider_abi_version == 3);
  assert(!engine->info().provider_name.empty());
  assert(!engine->info().provider_version.empty());
  assert_known_test_provider(engine->info().provider_name, engine->info().provider_capabilities);

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(runtime.media_engine_kind == "builtin_native_plugin");
  assert(!runtime.native_media_supported);
  assert(!runtime.native_media_active);
  assert(runtime.media_engine_state == "signaling_ready");
  assert(runtime.native_media_provider["abi_version"].asInt() == 3);
  Json::Value polled(Json::nullValue);
  assert(engine->poll_status(&polled, &err));
  assert(err.empty());
  assert(polled.isNull());
  assert(runtime.native_media_provider["name"].asString() == engine->info().provider_name);
  assert_known_test_provider(
    runtime.native_media_provider["name"].asString(),
    runtime.native_media_provider["capabilities"]);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = "remote-offer";
  ready.initial_remote_candidates.resize(1);

  agentd::VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  assert(answer.type == "answer");
  if (engine->info().provider_name == "agentd_builtin_sample_provider") {
    assert(answer.sdp == "agentd-builtin-sample-answer");
  } else {
    assert(!answer.sdp.empty());
    assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  }
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(runtime.media_engine_state == "answer_ready");
  assert(!runtime.native_media_active);
}

static void test_builtin_media_engine_legacy_v1_plugin_compatibility_defaults_metadata() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().media_engine_kind == "builtin_native_plugin");
  assert(!engine->info().native_media_supported);
  assert(engine->info().provider_abi_version == 1);
  assert(!engine->info().provider_name.empty());
  assert(engine->info().provider_version == "legacy_abi_v1");
  assert(engine->info().provider_capabilities["legacy_abi_v1"].asBool());

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(!runtime.native_media_supported);
  assert(!runtime.native_media_active);
  assert(runtime.native_media_provider["abi_version"].asInt() == 1);
  assert(runtime.native_media_provider["version"].asString() == "legacy_abi_v1");

  agentd::VoiceBrokerSignalDescription answer;
  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = "legacy-offer";
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  assert(answer.sdp == "legacy-v1-answer");
}

static void test_builtin_native_probe_json_reports_provider_details() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;

  const Json::Value probe = agentd::builtin_voice_peer_native_media_engine_probe_json(cfg);
  assert(probe["configured"].asBool());
  assert(probe["loadable"].asBool());
  assert(probe["media_engine_kind"].asString() == "builtin_native_plugin");
  assert(probe["native_media_supported"].asBool() == false);
  assert(probe["provider"]["abi_version"].asInt() == 3);
  assert(!probe["provider"]["name"].asString().empty());
  assert_known_test_provider(
    probe["provider"]["name"].asString(),
    probe["provider"]["capabilities"]);
}

}  // namespace

int main() {
  test_runtime_kind_info_reports_reserved_and_stub_modes();
  test_builtin_media_engine_stub_answers_remote_offer();
  test_builtin_media_engine_native_plugin_loads_sample_provider_metadata();
  test_builtin_media_engine_legacy_v1_plugin_compatibility_defaults_metadata();
  test_builtin_native_probe_json_reports_provider_details();
  return 0;
}
