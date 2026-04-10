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
    assert(capabilities["audio_decode"].asBool());
    assert(capabilities["audio_stage"].asBool());
    assert(capabilities["audio_drain"].asBool());
    assert(capabilities["audio_owner_handoff"].asBool());
    assert(capabilities["audio_submit"].asBool());
    assert(capabilities["audio_outbound_pcmu"].asBool());
    assert(capabilities["audio_outbound_pcma"].asBool());
    assert(capabilities["audio_codec_pcmu"].asBool());
    assert(capabilities["audio_codec_pcma"].asBool());
    assert(capabilities.isMember("audio_codec_opus"));
    assert(capabilities["ice"].asBool());
    assert(capabilities["dtls"].asBool());
    assert(capabilities["dtls_handshake"].asBool());
    assert(capabilities["dtls_srtp_export"].asBool());
    assert(capabilities["srtp_contexts"].asBool());
    assert(capabilities["srtp"].asBool());
    assert(capabilities["rtp_ingest"].asBool());
    assert(capabilities["rtp_transmit"].asBool());
    assert(capabilities["sctp"].asBool());
    assert(capabilities["real_media_engine"].asBool() == false);
    return;
  }
  assert(false && "unexpected builtin native provider");
}

static bool expected_native_supported_for_provider(const std::string& provider_name) {
  if (provider_name == "agentd_builtin_sample_provider") return false;
  if (provider_name == "agentd_builtin_embedded_transport_provider") return true;
  assert(false && "unexpected builtin native provider");
  return false;
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
  assert(native_info.native_media_supported ==
         expected_native_supported_for_provider(native_info.provider_name));
  assert(!native_info.native_media_active);
  assert(native_info.provider_abi_version == 5);
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
  assert(engine->info().native_media_supported ==
         expected_native_supported_for_provider(engine->info().provider_name));
  assert(!engine->info().native_media_active);
  assert(engine->info().provider_abi_version == 5);
  assert(!engine->info().provider_name.empty());
  assert(!engine->info().provider_version.empty());
  assert_known_test_provider(engine->info().provider_name, engine->info().provider_capabilities);

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(runtime.media_engine_kind == "builtin_native_plugin");
  assert(runtime.native_media_supported ==
         expected_native_supported_for_provider(engine->info().provider_name));
  assert(!runtime.native_media_active);
  assert(runtime.media_engine_state == "signaling_ready");
  assert(runtime.native_media_provider["abi_version"].asInt() == 5);
  Json::Value polled(Json::nullValue);
  assert(engine->poll_status(&polled, &err));
  assert(err.empty());
  assert(polled.isNull());
  agentd::VoicePeerBuiltinAudioChunk drained;
  Json::Value drained_event(Json::nullValue);
  assert(engine->drain_audio(&drained, &drained_event, &err));
  assert(err.empty());
  assert(drained.pcm_samples.empty());
  assert(drained_event.isNull());
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
  assert(runtime.audio_frames_decoded == 0);
  assert(runtime.audio_pcm_samples_decoded == 0);
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
  assert(probe["provider"]["abi_version"].asInt() == 5);
  assert(!probe["provider"]["name"].asString().empty());
  assert(probe["native_media_supported"].asBool() ==
         expected_native_supported_for_provider(probe["provider"]["name"].asString()));
  assert_known_test_provider(
    probe["provider"]["name"].asString(),
    probe["provider"]["capabilities"]);
}

static void test_media_engine_event_tracks_audio_drain_fields() {
  VoicePeerRuntime runtime;
  Json::Value payload(Json::objectValue);
  payload["event"] = "audio_chunk_processed";
  payload["media_engine_state"] = "media_active";
  payload["native_media_supported"] = true;
  payload["native_media_active"] = true;
  payload["rtp_packets_sent"] = Json::Int64(1);
  payload["rtp_payload_bytes_sent"] = Json::Int64(160);
  payload["rtp_last_sent_payload_type"] = Json::Int64(0);
  payload["rtp_last_sent_sequence"] = Json::Int64(12);
  payload["rtp_last_sent_timestamp"] = Json::Int64(320);
  payload["rtp_last_sent_ssrc"] = Json::Int64(0xA6E17D01);
  payload["audio_outbound_frames_sent"] = Json::Int64(1);
  payload["audio_pcm_samples_submitted_total"] = Json::Int64(160);
  payload["audio_last_outbound_samples"] = Json::Int64(160);
  payload["audio_outbound_payload_type"] = Json::Int64(8);
  payload["audio_outbound_codec_name"] = "PCMA";
  payload["audio_outbound_sample_rate_hz"] = Json::Int64(8000);
  payload["audio_outbound_channels"] = Json::Int64(1);
  payload["audio_drain_events_total"] = Json::Int64(3);
  payload["audio_pcm_samples_drained_total"] = Json::Int64(2880);
  payload["audio_pcm_samples_owned"] = Json::Int64(1440);
  payload["audio_last_drain_samples"] = Json::Int64(960);
  payload["audio_process_events_total"] = Json::Int64(2);
  payload["audio_pcm_samples_processed_total"] = Json::Int64(1920);
  payload["audio_last_process_samples"] = Json::Int64(480);
  payload["audio_last_peak_abs_pcm16"] = Json::Int64(12000);
  payload["audio_last_rms_pcm16"] = Json::Int64(8000);
  payload["audio_render_events_total"] = Json::Int64(2);
  payload["audio_pcm_samples_rendered_total"] = Json::Int64(1920);
  payload["audio_render_window_samples"] = Json::Int64(1920);
  payload["audio_last_render_samples"] = Json::Int64(480);
  payload["audio_playback_enabled"] = true;
  payload["audio_playback_stream_open"] = true;
  payload["audio_playback_events_total"] = Json::Int64(2);
  payload["audio_pcm_samples_played_total"] = Json::Int64(1440);
  payload["audio_pcm_samples_playback_queued"] = Json::Int64(480);
  payload["audio_last_playback_samples"] = Json::Int64(480);
  payload["audio_last_sample_rate_hz"] = Json::Int64(48000);
  payload["audio_last_channels"] = Json::Int64(2);
  payload["audio_last_frame_samples_per_channel"] = Json::Int64(480);
  payload["audio_last_codec_name"] = "PCMU";
  payload["audio_render_wav_path"] = "/tmp/audio_recent.wav";
  payload["audio_render_last_error"] = "";
  payload["audio_playback_device_name"] = "Built-in Output";
  payload["audio_playback_last_error"] = "";
  payload["audio_outbound_last_error"] = "";
  note_voice_peer_media_engine_event(&runtime, payload);
  assert(runtime.media_engine_state == "media_active");
  assert(runtime.native_media_supported);
  assert(runtime.native_media_active);
  assert(runtime.rtp_packets_sent == 1);
  assert(runtime.rtp_payload_bytes_sent == 160);
  assert(runtime.rtp_last_sent_payload_type == 0);
  assert(runtime.rtp_last_sent_sequence == 12);
  assert(runtime.rtp_last_sent_timestamp == 320);
  assert(runtime.rtp_last_sent_ssrc == 0xA6E17D01);
  assert(runtime.audio_outbound_frames_sent == 1);
  assert(runtime.audio_pcm_samples_submitted_total == 160);
  assert(runtime.audio_last_outbound_samples == 160);
  assert(runtime.audio_outbound_payload_type == 8);
  assert(runtime.audio_outbound_codec_name == "PCMA");
  assert(runtime.audio_outbound_sample_rate_hz == 8000);
  assert(runtime.audio_outbound_channels == 1);
  assert(runtime.audio_drain_events_total == 3);
  assert(runtime.audio_pcm_samples_drained_total == 2880);
  assert(runtime.audio_pcm_samples_owned == 1440);
  assert(runtime.audio_last_drain_samples == 960);
  assert(runtime.audio_process_events_total == 2);
  assert(runtime.audio_pcm_samples_processed_total == 1920);
  assert(runtime.audio_last_process_samples == 480);
  assert(runtime.audio_last_peak_abs_pcm16 == 12000);
  assert(runtime.audio_last_rms_pcm16 == 8000);
  assert(runtime.audio_render_events_total == 2);
  assert(runtime.audio_pcm_samples_rendered_total == 1920);
  assert(runtime.audio_render_window_samples == 1920);
  assert(runtime.audio_last_render_samples == 480);
  assert(runtime.audio_playback_enabled);
  assert(runtime.audio_playback_stream_open);
  assert(runtime.audio_playback_events_total == 2);
  assert(runtime.audio_pcm_samples_played_total == 1440);
  assert(runtime.audio_pcm_samples_playback_queued == 480);
  assert(runtime.audio_last_playback_samples == 480);
  assert(runtime.audio_last_sample_rate_hz == 48000);
  assert(runtime.audio_last_channels == 2);
  assert(runtime.audio_last_frame_samples_per_channel == 480);
  assert(runtime.audio_last_codec_name == "PCMU");
  assert(runtime.audio_render_wav_path == "/tmp/audio_recent.wav");
  assert(runtime.audio_render_last_error.empty());
  assert(runtime.audio_playback_device_name == "Built-in Output");
  assert(runtime.audio_playback_last_error.empty());
  assert(runtime.audio_outbound_last_error.empty());
}

}  // namespace

int main() {
  test_runtime_kind_info_reports_reserved_and_stub_modes();
  test_builtin_media_engine_stub_answers_remote_offer();
  test_builtin_media_engine_native_plugin_loads_sample_provider_metadata();
  test_builtin_media_engine_legacy_v1_plugin_compatibility_defaults_metadata();
  test_builtin_native_probe_json_reports_provider_details();
  test_media_engine_event_tracks_audio_drain_fields();
  return 0;
}
