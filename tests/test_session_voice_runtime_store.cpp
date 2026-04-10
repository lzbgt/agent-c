#include "agent_db.h"
#include "json_util.h"
#include "session_voice_runtime_store.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::VoicePeerRuntime;
using agentd::json_stringify;
using agentd::load_voice_peer_runtime_record;
using agentd::persist_voice_peer_runtime_record;
using agentd::recover_voice_peer_runtime_record;
using agentd::voice_peer_runtime_to_json;

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static void open_test_db(const std::filesystem::path& path, AgentDb* out_db) {
  assert(out_db);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::string err;
  const bool ok = out_db->open(path.string(), &err);
  assert(ok);
  (void)err;
}

static void test_persist_rejects_planned_runtime_preview() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_reject");
  AgentDb db;
  open_test_db(db_path, &db);

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";

  std::string err;
  assert(!persist_voice_peer_runtime_record(&db, st, &err));
  assert(err == "refusing to persist planned voice runtime preview");

  std::string raw;
  err.clear();
  assert(db.meta_get("session.voice_webrtc_peer.voice-sid", &raw, &err));
  assert(raw.empty());
#endif
}

static void test_load_self_heals_planned_runtime_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_load");
  AgentDb db;
  open_test_db(db_path, &db);

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.ready = false;
  st.running = false;

  std::string err;
  assert(db.meta_set("session.voice_webrtc_peer.voice-sid", json_stringify(voice_peer_runtime_to_json(st)), &err));

  bool self_healed = false;
  std::shared_ptr<VoicePeerRuntime> loaded;
  err.clear();
  assert(load_voice_peer_runtime_record(&db, "voice-sid", &loaded, &self_healed, &err));
  assert(!loaded);
  assert(self_healed);
  assert(err.empty());

  std::string raw;
  assert(db.meta_get("session.voice_webrtc_peer.voice-sid", &raw, &err));
  assert(raw.empty());
#endif
}

static void test_recover_reports_cleanup_for_planned_runtime_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_recover");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("voice_runtime_store_planned_state_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "voice_webrtc_peers" / "voice-sid";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stdout.jsonl").string().c_str(), "w");
    assert(f);
    std::fputs("{\"planned\":true}\n", f);
    std::fclose(f);
  }

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.stdout_log_path = (runtime_dir / "stdout.jsonl").string();
  st.stderr_log_path = (runtime_dir / "stderr.log").string();
  st.ready_file_path = (runtime_dir / "ready.json").string();

  std::string err;
  assert(db.meta_set("session.voice_webrtc_peer.voice-sid", json_stringify(voice_peer_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  std::shared_ptr<VoicePeerRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_voice_peer_runtime_record(cfg, &db, "voice-sid", &recovered, &updates, &err));
  assert(!recovered);
  assert(err.empty());
  assert(updates["cleanup_on_corrupt_record"]["persisted_record_cleared"].asBool());
  assert(updates["cleanup_on_corrupt_record"]["runtime_artifacts_deleted"].asBool());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_runtime_json_round_trips_media_engine_fields() {
  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.media_engine_kind = "builtin_signaling_stub";
  st.media_engine_state = "signaling_active";
  st.status_source = "memory";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.media_state_updated_unix_ms = 1234;
  st.media_events_total = 5;
  st.media_remote_offers_seen = 1;
  st.media_answers_sent = 1;
  st.media_remote_candidates_seen = 2;
  st.media_remote_byes_seen = 0;
  st.media_local_byes_sent = 1;
  st.native_media_supported = false;
  st.native_media_active = false;
  st.dtls_identity_ready = true;
  st.dtls_handshake_ready = true;
  st.dtls_exporter_ready = true;
  st.srtp_contexts_ready = true;
  st.srtp_inbound_ready = true;
  st.srtp_outbound_ready = true;
  st.dtls_fingerprint_sha256 = "AA:BB:CC";
  st.dtls_setup_role = "passive";
  st.dtls_certificate_subject = "/CN=agentd builtin embedded transport";
  st.dtls_handshake_state = "connected";
  st.dtls_selected_srtp_profile = "SRTP_AES128_CM_SHA1_80";
  st.srtp_last_error = "none";
  st.dtls_packets_sent = 12;
  st.dtls_packets_received = 9;
  st.rtp_packets_received = 3;
  st.rtp_payload_bytes_received = 480;
  st.rtp_packets_sent = 2;
  st.rtp_payload_bytes_sent = 320;
  st.rtp_last_payload_type = 96;
  st.rtp_last_sequence = 321;
  st.rtp_last_timestamp = 0x01020304;
  st.rtp_last_ssrc = 0x11223344;
  st.rtp_last_sent_payload_type = 0;
  st.rtp_last_sent_sequence = 22;
  st.rtp_last_sent_timestamp = 0x05060708;
  st.rtp_last_sent_ssrc = 0xA6E17D01;
  st.audio_frames_decoded = 7;
  st.audio_pcm_samples_decoded = 6720;
  st.audio_pcm_samples_buffered = 1920;
  st.audio_outbound_frames_sent = 2;
  st.audio_pcm_samples_submitted_total = 320;
  st.audio_last_outbound_samples = 160;
  st.audio_drain_events_total = 4;
  st.audio_pcm_samples_drained_total = 4800;
  st.audio_pcm_samples_owned = 2880;
  st.audio_last_drain_samples = 960;
  st.audio_process_events_total = 3;
  st.audio_pcm_samples_processed_total = 3360;
  st.audio_last_process_samples = 480;
  st.audio_last_peak_abs_pcm16 = 14000;
  st.audio_last_rms_pcm16 = 9000;
  st.audio_render_events_total = 3;
  st.audio_pcm_samples_rendered_total = 3360;
  st.audio_render_window_samples = 1920;
  st.audio_last_render_samples = 480;
  st.audio_playback_enabled = true;
  st.audio_playback_stream_open = true;
  st.audio_playback_events_total = 2;
  st.audio_pcm_samples_played_total = 2400;
  st.audio_pcm_samples_playback_queued = 480;
  st.audio_last_playback_samples = 960;
  st.audio_last_sample_rate_hz = 48000;
  st.audio_last_channels = 2;
  st.audio_last_frame_samples_per_channel = 960;
  st.audio_last_codec_name = "OPUS";
  st.audio_last_error = "decoder_warmup";
  st.audio_outbound_last_error = "outbound_warmup";
  st.audio_render_wav_path = "/tmp/agentd/voice-sid/audio_recent.wav";
  st.audio_render_last_error = "none";
  st.audio_playback_device_name = "Built-in Output";
  st.audio_playback_last_error = "";
  st.native_media_provider["abi_version"] = 5;
  st.native_media_provider["name"] = "agentd_builtin_sample_provider";
  st.native_media_provider["capabilities"]["transport_family"] = "sample_webrtc";
  st.native_media_provider["capabilities"]["sample_provider"] = true;

  const Json::Value json = voice_peer_runtime_to_json(st);
  assert(json["media_engine_kind"].asString() == "builtin_signaling_stub");
  assert(json["media_engine_state"].asString() == "signaling_active");
  assert(json["media_state_updated_unix_ms"].asInt64() == 1234);
  assert(json["media_events_total"].asInt64() == 5);
  assert(json["media_remote_offers_seen"].asInt64() == 1);
  assert(json["media_answers_sent"].asInt64() == 1);
  assert(json["media_remote_candidates_seen"].asInt64() == 2);
  assert(json["media_remote_byes_seen"].asInt64() == 0);
  assert(json["media_local_byes_sent"].asInt64() == 1);
  assert(json["native_media_supported"].asBool() == false);
  assert(json["native_media_active"].asBool() == false);
  assert(json["dtls_identity_ready"].asBool());
  assert(json["dtls_handshake_ready"].asBool());
  assert(json["dtls_exporter_ready"].asBool());
  assert(json["srtp_contexts_ready"].asBool());
  assert(json["srtp_inbound_ready"].asBool());
  assert(json["srtp_outbound_ready"].asBool());
  assert(json["dtls_fingerprint_sha256"].asString() == "AA:BB:CC");
  assert(json["dtls_setup_role"].asString() == "passive");
  assert(json["dtls_certificate_subject"].asString() == "/CN=agentd builtin embedded transport");
  assert(json["dtls_handshake_state"].asString() == "connected");
  assert(json["dtls_selected_srtp_profile"].asString() == "SRTP_AES128_CM_SHA1_80");
  assert(json["srtp_last_error"].asString() == "none");
  assert(json["dtls_packets_sent"].asInt64() == 12);
  assert(json["dtls_packets_received"].asInt64() == 9);
  assert(json["rtp_packets_received"].asInt64() == 3);
  assert(json["rtp_payload_bytes_received"].asInt64() == 480);
  assert(json["rtp_packets_sent"].asInt64() == 2);
  assert(json["rtp_payload_bytes_sent"].asInt64() == 320);
  assert(json["rtp_last_payload_type"].asInt64() == 96);
  assert(json["rtp_last_sequence"].asInt64() == 321);
  assert(json["rtp_last_timestamp"].asInt64() == 0x01020304);
  assert(json["rtp_last_ssrc"].asInt64() == 0x11223344);
  assert(json["rtp_last_sent_payload_type"].asInt64() == 0);
  assert(json["rtp_last_sent_sequence"].asInt64() == 22);
  assert(json["rtp_last_sent_timestamp"].asInt64() == 0x05060708);
  assert(json["rtp_last_sent_ssrc"].asInt64() == 0xA6E17D01);
  assert(json["audio_frames_decoded"].asInt64() == 7);
  assert(json["audio_pcm_samples_decoded"].asInt64() == 6720);
  assert(json["audio_pcm_samples_buffered"].asInt64() == 1920);
  assert(json["audio_outbound_frames_sent"].asInt64() == 2);
  assert(json["audio_pcm_samples_submitted_total"].asInt64() == 320);
  assert(json["audio_last_outbound_samples"].asInt64() == 160);
  assert(json["audio_drain_events_total"].asInt64() == 4);
  assert(json["audio_pcm_samples_drained_total"].asInt64() == 4800);
  assert(json["audio_pcm_samples_owned"].asInt64() == 2880);
  assert(json["audio_last_drain_samples"].asInt64() == 960);
  assert(json["audio_process_events_total"].asInt64() == 3);
  assert(json["audio_pcm_samples_processed_total"].asInt64() == 3360);
  assert(json["audio_last_process_samples"].asInt64() == 480);
  assert(json["audio_last_peak_abs_pcm16"].asInt64() == 14000);
  assert(json["audio_last_rms_pcm16"].asInt64() == 9000);
  assert(json["audio_render_events_total"].asInt64() == 3);
  assert(json["audio_pcm_samples_rendered_total"].asInt64() == 3360);
  assert(json["audio_render_window_samples"].asInt64() == 1920);
  assert(json["audio_last_render_samples"].asInt64() == 480);
  assert(json["audio_playback_enabled"].asBool() == true);
  assert(json["audio_playback_stream_open"].asBool() == true);
  assert(json["audio_playback_events_total"].asInt64() == 2);
  assert(json["audio_pcm_samples_played_total"].asInt64() == 2400);
  assert(json["audio_pcm_samples_playback_queued"].asInt64() == 480);
  assert(json["audio_last_playback_samples"].asInt64() == 960);
  assert(json["audio_last_sample_rate_hz"].asInt64() == 48000);
  assert(json["audio_last_channels"].asInt64() == 2);
  assert(json["audio_last_frame_samples_per_channel"].asInt64() == 960);
  assert(json["audio_last_codec_name"].asString() == "OPUS");
  assert(json["audio_last_error"].asString() == "decoder_warmup");
  assert(json["audio_outbound_last_error"].asString() == "outbound_warmup");
  assert(json["audio_render_wav_path"].asString() == "/tmp/agentd/voice-sid/audio_recent.wav");
  assert(json["audio_render_last_error"].asString() == "none");
  assert(json["audio_playback_device_name"].asString() == "Built-in Output");
  assert(json["native_media_provider"]["abi_version"].asInt() == 5);
  assert(json["native_media_provider"]["name"].asString() == "agentd_builtin_sample_provider");

  VoicePeerRuntime round_trip;
  std::string err;
  assert(agentd::voice_peer_runtime_from_json(json, &round_trip, &err));
  assert(err.empty());
  assert(round_trip.media_engine_kind == "builtin_signaling_stub");
  assert(round_trip.media_engine_state == "signaling_active");
  assert(round_trip.media_state_updated_unix_ms == 1234);
  assert(round_trip.media_events_total == 5);
  assert(round_trip.media_remote_offers_seen == 1);
  assert(round_trip.media_answers_sent == 1);
  assert(round_trip.media_remote_candidates_seen == 2);
  assert(round_trip.media_remote_byes_seen == 0);
  assert(round_trip.media_local_byes_sent == 1);
  assert(round_trip.native_media_supported == false);
  assert(round_trip.native_media_active == false);
  assert(round_trip.dtls_identity_ready);
  assert(round_trip.dtls_handshake_ready);
  assert(round_trip.dtls_exporter_ready);
  assert(round_trip.srtp_contexts_ready);
  assert(round_trip.srtp_inbound_ready);
  assert(round_trip.srtp_outbound_ready);
  assert(round_trip.dtls_fingerprint_sha256 == "AA:BB:CC");
  assert(round_trip.dtls_setup_role == "passive");
  assert(round_trip.dtls_certificate_subject == "/CN=agentd builtin embedded transport");
  assert(round_trip.dtls_handshake_state == "connected");
  assert(round_trip.dtls_selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");
  assert(round_trip.srtp_last_error == "none");
  assert(round_trip.dtls_packets_sent == 12);
  assert(round_trip.dtls_packets_received == 9);
  assert(round_trip.rtp_packets_received == 3);
  assert(round_trip.rtp_payload_bytes_received == 480);
  assert(round_trip.rtp_packets_sent == 2);
  assert(round_trip.rtp_payload_bytes_sent == 320);
  assert(round_trip.rtp_last_payload_type == 96);
  assert(round_trip.rtp_last_sequence == 321);
  assert(round_trip.rtp_last_timestamp == 0x01020304);
  assert(round_trip.rtp_last_ssrc == 0x11223344);
  assert(round_trip.rtp_last_sent_payload_type == 0);
  assert(round_trip.rtp_last_sent_sequence == 22);
  assert(round_trip.rtp_last_sent_timestamp == 0x05060708);
  assert(round_trip.rtp_last_sent_ssrc == 0xA6E17D01);
  assert(round_trip.audio_frames_decoded == 7);
  assert(round_trip.audio_pcm_samples_decoded == 6720);
  assert(round_trip.audio_pcm_samples_buffered == 1920);
  assert(round_trip.audio_outbound_frames_sent == 2);
  assert(round_trip.audio_pcm_samples_submitted_total == 320);
  assert(round_trip.audio_last_outbound_samples == 160);
  assert(round_trip.audio_drain_events_total == 4);
  assert(round_trip.audio_pcm_samples_drained_total == 4800);
  assert(round_trip.audio_pcm_samples_owned == 2880);
  assert(round_trip.audio_last_drain_samples == 960);
  assert(round_trip.audio_process_events_total == 3);
  assert(round_trip.audio_pcm_samples_processed_total == 3360);
  assert(round_trip.audio_last_process_samples == 480);
  assert(round_trip.audio_last_peak_abs_pcm16 == 14000);
  assert(round_trip.audio_last_rms_pcm16 == 9000);
  assert(round_trip.audio_render_events_total == 3);
  assert(round_trip.audio_pcm_samples_rendered_total == 3360);
  assert(round_trip.audio_render_window_samples == 1920);
  assert(round_trip.audio_last_render_samples == 480);
  assert(round_trip.audio_playback_enabled == true);
  assert(round_trip.audio_playback_stream_open == true);
  assert(round_trip.audio_playback_events_total == 2);
  assert(round_trip.audio_pcm_samples_played_total == 2400);
  assert(round_trip.audio_pcm_samples_playback_queued == 480);
  assert(round_trip.audio_last_playback_samples == 960);
  assert(round_trip.audio_last_sample_rate_hz == 48000);
  assert(round_trip.audio_last_channels == 2);
  assert(round_trip.audio_last_frame_samples_per_channel == 960);
  assert(round_trip.audio_last_codec_name == "OPUS");
  assert(round_trip.audio_last_error == "decoder_warmup");
  assert(round_trip.audio_outbound_last_error == "outbound_warmup");
  assert(round_trip.audio_render_wav_path == "/tmp/agentd/voice-sid/audio_recent.wav");
  assert(round_trip.audio_render_last_error == "none");
  assert(round_trip.audio_playback_device_name == "Built-in Output");
  assert(round_trip.native_media_provider["abi_version"].asInt() == 5);
  assert(round_trip.native_media_provider["capabilities"]["transport_family"].asString() == "sample_webrtc");
  assert(round_trip.native_media_provider["capabilities"]["sample_provider"].asBool());
}

}  // namespace

int main() {
  test_persist_rejects_planned_runtime_preview();
  test_load_self_heals_planned_runtime_record();
  test_recover_reports_cleanup_for_planned_runtime_record();
  test_runtime_json_round_trips_media_engine_fields();
  return 0;
}
