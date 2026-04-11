#include "session_voice_builtin_embedded_status.h"

#include <cassert>
#include <string>

namespace {

bool contains(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

void test_json_string_escape() {
  assert(agentd::escape_builtin_embedded_media_json_string("a\"b\\c\n\r\t") ==
         "a\\\"b\\\\c\\n\\r\\t");
}

void test_derived_media_engine_state_order() {
  agentd::BuiltinVoiceAsyncProgressKey progress;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "signaling_ready");

  progress.remote_description_applied = true;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "signaling_active");

  progress.transport_connectivity_ready = true;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "transport_connected");

  progress.dtls_handshake_ready = true;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "dtls_connected");

  progress.srtp_contexts_ready = true;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "media_transport_ready");

  progress.rtp_packets_sent = 1;
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "media_active");

  progress.dtls_handshake_state = "failed";
  assert(agentd::derive_builtin_embedded_media_engine_state(progress) == "failed");
}

void test_event_json_includes_required_and_optional_fields() {
  agentd::BuiltinEmbeddedMediaStatusSnapshot snapshot;
  snapshot.event_name = "embedded_transport_progress";
  snapshot.media_engine_state = "media_active";
  snapshot.provider_name = "provider\"x";
  snapshot.transport_family = "embedded_transport_primitives";
  snapshot.srtp_version = "libsrtp-test";
  snapshot.dtls_setup_role = "passive";
  snapshot.dtls_fingerprint_sha256 = "AA:BB";
  snapshot.dtls_certificate_subject = "CN=agentd";
  snapshot.dtls_last_error = "dtls \"error\"";
  snapshot.sdp_answer_shape = "active";
  snapshot.local_description_bytes = 321;
  snapshot.dtls_packets_sent = 2;
  snapshot.dtls_packets_received = 3;
  snapshot.initial_remote_candidate_count = 4;
  snapshot.usrsctp_initialized = false;

  snapshot.progress.libjuice_state = "connected";
  snapshot.progress.dtls_handshake_state = "ready";
  snapshot.progress.dtls_selected_srtp_profile = "SRTP_AES128_CM_SHA1_80";
  snapshot.progress.srtp_last_error = "srtp_error";
  snapshot.progress.last_remote_description_error = "remote_error";
  snapshot.progress.selected_local_candidate = "local-cand";
  snapshot.progress.selected_remote_candidate = "remote-cand";
  snapshot.progress.selected_local_address = "127.0.0.1:1";
  snapshot.progress.selected_remote_address = "127.0.0.1:2";
  snapshot.progress.offers_seen = 5;
  snapshot.progress.remote_candidates_seen = 6;
  snapshot.progress.local_candidates_observed = 7;
  snapshot.progress.gather_started = true;
  snapshot.progress.gathering_done = true;
  snapshot.progress.remote_description_applied = true;
  snapshot.progress.transport_connectivity_ready = true;
  snapshot.progress.dtls_identity_ready = true;
  snapshot.progress.dtls_handshake_ready = true;
  snapshot.progress.dtls_exporter_ready = true;
  snapshot.progress.srtp_contexts_ready = true;
  snapshot.progress.srtp_inbound_ready = true;
  snapshot.progress.srtp_outbound_ready = true;
  snapshot.progress.rtp_packets_received = 8;
  snapshot.progress.rtp_payload_bytes_received = 9;
  snapshot.progress.rtp_packets_sent = 10;
  snapshot.progress.rtp_payload_bytes_sent = 11;
  snapshot.progress.rtcp_packets_received = 12;
  snapshot.progress.rtcp_packets_sent = 13;
  snapshot.progress.rtcp_payload_bytes_received = 14;
  snapshot.progress.rtcp_payload_bytes_sent = 15;
  snapshot.progress.rtcp_sender_reports_sent = 16;
  snapshot.progress.rtcp_receiver_reports_sent = 17;
  snapshot.progress.rtcp_receiver_report_blocks_sent = 18;
  snapshot.progress.rtp_last_payload_type = 111;
  snapshot.progress.rtp_last_sequence = 22;
  snapshot.progress.rtp_last_timestamp = 333;
  snapshot.progress.rtp_last_ssrc = 444;
  snapshot.progress.rtp_last_sent_payload_type = 0;
  snapshot.progress.rtp_last_sent_sequence = 23;
  snapshot.progress.rtp_last_sent_timestamp = 555;
  snapshot.progress.rtp_last_sent_ssrc = 666;
  snapshot.progress.rtcp_last_packet_type = 200;
  snapshot.progress.rtcp_last_ssrc = 777;
  snapshot.progress.rtcp_last_sent_packet_type = 201;
  snapshot.progress.rtcp_last_sent_ssrc = 888;
  snapshot.progress.rtcp_last_reported_rtp_ssrc = 999;
  snapshot.progress.rtcp_last_report_fraction_lost = 1;
  snapshot.progress.rtcp_last_report_cumulative_lost = 2;
  snapshot.progress.rtcp_last_report_highest_sequence = 3;
  snapshot.progress.rtcp_last_report_jitter = 4;
  snapshot.progress.rtcp_last_report_lsr = 5;
  snapshot.progress.rtcp_last_report_dlsr = 6;
  snapshot.progress.audio_frames_decoded = 19;
  snapshot.progress.audio_pcm_samples_decoded = 20;
  snapshot.progress.audio_pcm_samples_buffered = 21;
  snapshot.progress.audio_outbound_frames_sent = 22;
  snapshot.progress.audio_pcm_samples_submitted_total = 23;
  snapshot.progress.audio_last_outbound_samples = 24;
  snapshot.progress.audio_outbound_payload_type = 8;
  snapshot.progress.audio_outbound_sample_rate_hz = 8000;
  snapshot.progress.audio_outbound_channels = 1;
  snapshot.progress.audio_outbound_codec_name = "PCMA";
  snapshot.progress.audio_last_sample_rate_hz = 48000;
  snapshot.progress.audio_last_channels = 2;
  snapshot.progress.audio_last_frame_samples_per_channel = 960;
  snapshot.progress.audio_last_codec_name = "OPUS";
  snapshot.progress.audio_last_error = "decode_error";
  snapshot.progress.audio_outbound_last_error = "encode_error";
  snapshot.progress.rtcp_last_error = "rtcp_error";

  const std::string json = agentd::build_builtin_embedded_media_event_json(snapshot);
  assert(contains(json, "\"ok\":true"));
  assert(contains(json, "\"event\":\"embedded_transport_progress\""));
  assert(contains(json, "\"media_engine_state\":\"media_active\""));
  assert(contains(json, "\"media_engine_kind\":\"builtin_native_plugin\""));
  assert(contains(json, "\"native_media_supported\":true"));
  assert(contains(json, "\"native_media_active\":true"));
  assert(contains(json, "\"provider\":\"provider\\\"x\""));
  assert(contains(json, "\"transport_family\":\"embedded_transport_primitives\""));
  assert(contains(json, "\"dtls_identity_ready\":true"));
  assert(contains(json, "\"dtls_handshake_ready\":true"));
  assert(contains(json, "\"dtls_exporter_ready\":true"));
  assert(contains(json, "\"srtp_contexts_ready\":true"));
  assert(contains(json, "\"srtp_inbound_ready\":true"));
  assert(contains(json, "\"srtp_outbound_ready\":true"));
  assert(contains(json, "\"dtls_setup_role\":\"passive\""));
  assert(contains(json, "\"dtls_handshake_state\":\"ready\""));
  assert(contains(json, "\"sdp_answer_shape\":\"active\""));
  assert(contains(json, "\"libjuice_state\":\"connected\""));
  assert(contains(json, "\"libjuice_local_description_bytes\":321"));
  assert(contains(json, "\"dtls_packets_sent\":2"));
  assert(contains(json, "\"dtls_packets_received\":3"));
  assert(contains(json, "\"rtp_packets_received\":8"));
  assert(contains(json, "\"rtp_payload_bytes_received\":9"));
  assert(contains(json, "\"rtp_packets_sent\":10"));
  assert(contains(json, "\"rtp_payload_bytes_sent\":11"));
  assert(contains(json, "\"rtcp_packets_received\":12"));
  assert(contains(json, "\"rtcp_payload_bytes_sent\":15"));
  assert(contains(json, "\"rtcp_sender_reports_sent\":16"));
  assert(contains(json, "\"rtcp_receiver_report_blocks_sent\":18"));
  assert(contains(json, "\"audio_frames_decoded\":19"));
  assert(contains(json, "\"audio_pcm_samples_submitted_total\":23"));
  assert(contains(json, "\"initial_remote_candidate_count\":4"));
  assert(contains(json, "\"gather_started\":true"));
  assert(contains(json, "\"gathering_done\":true"));
  assert(contains(json, "\"remote_description_applied\":true"));
  assert(contains(json, "\"transport_connectivity_ready\":true"));
  assert(contains(json, "\"srtp_version\":\"libsrtp-test\""));
  assert(contains(json, "\"usrsctp_initialized\":false"));
  assert(contains(json, "\"dtls_fingerprint_sha256\":\"AA:BB\""));
  assert(contains(json, "\"dtls_certificate_subject\":\"CN=agentd\""));
  assert(contains(json, "\"dtls_selected_srtp_profile\":\"SRTP_AES128_CM_SHA1_80\""));
  assert(contains(json, "\"dtls_last_error\":\"dtls \\\"error\\\"\""));
  assert(contains(json, "\"srtp_last_error\":\"srtp_error\""));
  assert(contains(json, "\"rtp_last_payload_type\":111"));
  assert(contains(json, "\"rtp_last_sent_payload_type\":0"));
  assert(contains(json, "\"rtp_last_sent_timestamp\":555"));
  assert(contains(json, "\"rtcp_last_reported_rtp_ssrc\":999"));
  assert(contains(json, "\"rtcp_last_report_cumulative_lost\":2"));
  assert(contains(json, "\"audio_outbound_payload_type\":8"));
  assert(contains(json, "\"audio_outbound_codec_name\":\"PCMA\""));
  assert(contains(json, "\"audio_last_frame_samples_per_channel\":960"));
  assert(contains(json, "\"audio_last_codec_name\":\"OPUS\""));
  assert(contains(json, "\"audio_last_error\":\"decode_error\""));
  assert(contains(json, "\"audio_outbound_last_error\":\"encode_error\""));
  assert(contains(json, "\"rtcp_last_error\":\"rtcp_error\""));
  assert(contains(json, "\"libjuice_selected_local_candidate\":\"local-cand\""));
  assert(contains(json, "\"libjuice_selected_remote_address\":\"127.0.0.1:2\""));
  assert(contains(json, "\"remote_description_error\":\"remote_error\""));
}

void test_event_json_omits_optional_fields_when_absent() {
  agentd::BuiltinEmbeddedMediaStatusSnapshot snapshot;
  snapshot.event_name = "media_engine_initialized";
  snapshot.media_engine_state = "signaling_ready";
  snapshot.progress.libjuice_state = "disconnected";
  snapshot.progress.dtls_handshake_state = "idle";
  const std::string json = agentd::build_builtin_embedded_media_event_json(snapshot);
  assert(contains(json, "\"native_media_active\":false"));
  assert(!contains(json, "audio_outbound_payload_type"));
  assert(!contains(json, "rtcp_last_reported_rtp_ssrc"));
  assert(!contains(json, "remote_description_error"));
}

}  // namespace

int main() {
  test_json_string_escape();
  test_derived_media_engine_state_order();
  test_event_json_includes_required_and_optional_fields();
  test_event_json_omits_optional_fields_when_absent();
  return 0;
}
