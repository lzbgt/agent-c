#include "session_voice_builtin_embedded_status.h"

namespace agentd {
namespace {

bool native_media_active(const BuiltinVoiceAsyncProgressKey& progress) {
  return progress.rtp_packets_received > 0 ||
         progress.rtp_packets_sent > 0 ||
         progress.rtcp_packets_received > 0 ||
         progress.rtcp_packets_sent > 0;
}

void append_json_string_field(
  std::string* json,
  const char* name,
  const std::string& value
) {
  if (!json || !name) return;
  *json += ",\"";
  *json += name;
  *json += "\":\"";
  *json += escape_builtin_embedded_media_json_string(value);
  *json += "\"";
}

void append_json_int_field(std::string* json, const char* name, int64_t value) {
  if (!json || !name) return;
  *json += ",\"";
  *json += name;
  *json += "\":";
  *json += std::to_string(value);
}

void append_json_uint_field(std::string* json, const char* name, uint64_t value) {
  if (!json || !name) return;
  *json += ",\"";
  *json += name;
  *json += "\":";
  *json += std::to_string(value);
}

void append_json_bool_field(std::string* json, const char* name, bool value) {
  if (!json || !name) return;
  *json += ",\"";
  *json += name;
  *json += "\":";
  *json += value ? "true" : "false";
}

void append_json_string_field_if_present(
  std::string* json,
  const char* name,
  const std::string& value
) {
  if (value.empty()) return;
  append_json_string_field(json, name, value);
}

void append_json_int_field_if_nonnegative(
  std::string* json,
  const char* name,
  int64_t value
) {
  if (value < 0) return;
  append_json_int_field(json, name, value);
}

void append_json_uint_field_if_positive(
  std::string* json,
  const char* name,
  uint64_t value
) {
  if (value == 0) return;
  append_json_uint_field(json, name, value);
}

void append_json_int_field_if_positive(
  std::string* json,
  const char* name,
  int64_t value
) {
  if (value <= 0) return;
  append_json_int_field(json, name, value);
}

}  // namespace

std::string escape_builtin_embedded_media_json_string(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string derive_builtin_embedded_media_engine_state(
  const BuiltinVoiceAsyncProgressKey& progress
) {
  if (progress.dtls_handshake_state == "failed") return "failed";
  if (native_media_active(progress)) return "media_active";
  if (progress.srtp_contexts_ready) return "media_transport_ready";
  if (progress.dtls_handshake_ready) return "dtls_connected";
  if (progress.transport_connectivity_ready) return "transport_connected";
  if (progress.remote_description_applied) return "signaling_active";
  return "signaling_ready";
}

std::string build_builtin_embedded_media_event_json(
  const BuiltinEmbeddedMediaStatusSnapshot& snapshot
) {
  const BuiltinVoiceAsyncProgressKey& p = snapshot.progress;
  std::string json =
    std::string("{\"ok\":true,\"event\":\"") +
    escape_builtin_embedded_media_json_string(snapshot.event_name) +
    "\",\"media_engine_state\":\"" +
    escape_builtin_embedded_media_json_string(snapshot.media_engine_state) +
    "\",\"media_engine_kind\":\"builtin_native_plugin\"";
  append_json_bool_field(&json, "native_media_supported", true);
  append_json_bool_field(&json, "native_media_active", native_media_active(p));
  append_json_string_field(&json, "provider", snapshot.provider_name);
  append_json_string_field(&json, "transport_family", snapshot.transport_family);
  append_json_bool_field(&json, "dtls_identity_ready", p.dtls_identity_ready);
  append_json_bool_field(&json, "dtls_handshake_ready", p.dtls_handshake_ready);
  append_json_bool_field(&json, "dtls_exporter_ready", p.dtls_exporter_ready);
  append_json_bool_field(&json, "srtp_contexts_ready", p.srtp_contexts_ready);
  append_json_bool_field(&json, "srtp_inbound_ready", p.srtp_inbound_ready);
  append_json_bool_field(&json, "srtp_outbound_ready", p.srtp_outbound_ready);
  append_json_string_field(&json, "dtls_setup_role", snapshot.dtls_setup_role);
  append_json_string_field(&json, "dtls_handshake_state", p.dtls_handshake_state);
  append_json_string_field(&json, "sdp_answer_shape", snapshot.sdp_answer_shape);
  append_json_string_field(&json, "libjuice_state", p.libjuice_state);
  append_json_uint_field(
    &json,
    "libjuice_local_description_bytes",
    static_cast<uint64_t>(snapshot.local_description_bytes));
  append_json_uint_field(&json, "dtls_packets_sent", snapshot.dtls_packets_sent);
  append_json_uint_field(&json, "dtls_packets_received", snapshot.dtls_packets_received);
  append_json_uint_field(&json, "rtp_packets_received", p.rtp_packets_received);
  append_json_uint_field(&json, "rtp_payload_bytes_received", p.rtp_payload_bytes_received);
  append_json_uint_field(&json, "rtp_packets_sent", p.rtp_packets_sent);
  append_json_uint_field(&json, "rtp_payload_bytes_sent", p.rtp_payload_bytes_sent);
  append_json_uint_field(&json, "rtcp_packets_received", p.rtcp_packets_received);
  append_json_uint_field(&json, "rtcp_packets_sent", p.rtcp_packets_sent);
  append_json_uint_field(&json, "rtcp_payload_bytes_received", p.rtcp_payload_bytes_received);
  append_json_uint_field(&json, "rtcp_payload_bytes_sent", p.rtcp_payload_bytes_sent);
  append_json_uint_field(&json, "rtcp_sender_reports_sent", p.rtcp_sender_reports_sent);
  append_json_uint_field(&json, "rtcp_receiver_reports_sent", p.rtcp_receiver_reports_sent);
  append_json_uint_field(
    &json,
    "rtcp_receiver_report_blocks_sent",
    p.rtcp_receiver_report_blocks_sent);
  append_json_uint_field(&json, "audio_frames_decoded", p.audio_frames_decoded);
  append_json_uint_field(&json, "audio_pcm_samples_decoded", p.audio_pcm_samples_decoded);
  append_json_uint_field(&json, "audio_pcm_samples_buffered", p.audio_pcm_samples_buffered);
  append_json_uint_field(&json, "audio_outbound_frames_sent", p.audio_outbound_frames_sent);
  append_json_uint_field(
    &json,
    "audio_pcm_samples_submitted_total",
    p.audio_pcm_samples_submitted_total);
  append_json_uint_field(&json, "audio_last_outbound_samples", p.audio_last_outbound_samples);
  append_json_uint_field(&json, "local_candidates_observed", p.local_candidates_observed);
  append_json_uint_field(&json, "remote_candidates_seen", p.remote_candidates_seen);
  append_json_uint_field(&json, "offers_seen", p.offers_seen);
  append_json_uint_field(
    &json,
    "initial_remote_candidate_count",
    snapshot.initial_remote_candidate_count);
  append_json_bool_field(&json, "gather_started", p.gather_started);
  append_json_bool_field(&json, "remote_description_applied", p.remote_description_applied);
  append_json_bool_field(&json, "gathering_done", p.gathering_done);
  append_json_bool_field(&json, "transport_connectivity_ready", p.transport_connectivity_ready);
  append_json_string_field(&json, "srtp_version", snapshot.srtp_version);
  append_json_bool_field(&json, "usrsctp_initialized", snapshot.usrsctp_initialized);

  append_json_string_field_if_present(
    &json,
    "dtls_fingerprint_sha256",
    snapshot.dtls_fingerprint_sha256);
  append_json_string_field_if_present(
    &json,
    "dtls_certificate_subject",
    snapshot.dtls_certificate_subject);
  append_json_string_field_if_present(
    &json,
    "dtls_selected_srtp_profile",
    p.dtls_selected_srtp_profile);
  append_json_string_field_if_present(&json, "dtls_last_error", snapshot.dtls_last_error);
  append_json_string_field_if_present(&json, "srtp_last_error", p.srtp_last_error);

  append_json_int_field_if_nonnegative(&json, "rtp_last_payload_type", p.rtp_last_payload_type);
  append_json_int_field_if_nonnegative(&json, "rtp_last_sequence", p.rtp_last_sequence);
  append_json_uint_field_if_positive(&json, "rtp_last_timestamp", p.rtp_last_timestamp);
  append_json_uint_field_if_positive(&json, "rtp_last_ssrc", p.rtp_last_ssrc);
  append_json_int_field_if_nonnegative(
    &json,
    "rtp_last_sent_payload_type",
    p.rtp_last_sent_payload_type);
  append_json_int_field_if_nonnegative(
    &json,
    "rtp_last_sent_sequence",
    p.rtp_last_sent_sequence);
  if (p.rtp_last_sent_timestamp > 0 || p.rtp_packets_sent > 0) {
    append_json_uint_field(&json, "rtp_last_sent_timestamp", p.rtp_last_sent_timestamp);
  }
  append_json_uint_field_if_positive(&json, "rtp_last_sent_ssrc", p.rtp_last_sent_ssrc);
  append_json_int_field_if_nonnegative(&json, "rtcp_last_packet_type", p.rtcp_last_packet_type);
  append_json_uint_field_if_positive(&json, "rtcp_last_ssrc", p.rtcp_last_ssrc);
  append_json_int_field_if_nonnegative(
    &json,
    "rtcp_last_sent_packet_type",
    p.rtcp_last_sent_packet_type);
  append_json_uint_field_if_positive(&json, "rtcp_last_sent_ssrc", p.rtcp_last_sent_ssrc);
  if (p.rtcp_last_reported_rtp_ssrc > 0) {
    append_json_uint_field(
      &json,
      "rtcp_last_reported_rtp_ssrc",
      p.rtcp_last_reported_rtp_ssrc);
    append_json_int_field(
      &json,
      "rtcp_last_report_fraction_lost",
      p.rtcp_last_report_fraction_lost);
    append_json_int_field(
      &json,
      "rtcp_last_report_cumulative_lost",
      p.rtcp_last_report_cumulative_lost);
    append_json_uint_field(
      &json,
      "rtcp_last_report_highest_sequence",
      p.rtcp_last_report_highest_sequence);
    append_json_uint_field(&json, "rtcp_last_report_jitter", p.rtcp_last_report_jitter);
    append_json_uint_field(&json, "rtcp_last_report_lsr", p.rtcp_last_report_lsr);
    append_json_uint_field(&json, "rtcp_last_report_dlsr", p.rtcp_last_report_dlsr);
  }
  append_json_int_field_if_nonnegative(
    &json,
    "audio_outbound_payload_type",
    p.audio_outbound_payload_type);
  append_json_string_field_if_present(
    &json,
    "audio_outbound_codec_name",
    p.audio_outbound_codec_name);
  append_json_int_field_if_positive(
    &json,
    "audio_outbound_sample_rate_hz",
    p.audio_outbound_sample_rate_hz);
  append_json_int_field_if_positive(
    &json,
    "audio_outbound_channels",
    p.audio_outbound_channels);
  append_json_int_field_if_positive(
    &json,
    "audio_last_sample_rate_hz",
    p.audio_last_sample_rate_hz);
  append_json_int_field_if_positive(
    &json,
    "audio_last_channels",
    p.audio_last_channels);
  append_json_int_field_if_positive(
    &json,
    "audio_last_frame_samples_per_channel",
    p.audio_last_frame_samples_per_channel);
  append_json_string_field_if_present(&json, "audio_last_codec_name", p.audio_last_codec_name);
  append_json_string_field_if_present(&json, "audio_last_error", p.audio_last_error);
  append_json_string_field_if_present(
    &json,
    "audio_outbound_last_error",
    p.audio_outbound_last_error);
  append_json_string_field_if_present(&json, "rtcp_last_error", p.rtcp_last_error);
  append_json_string_field_if_present(
    &json,
    "libjuice_selected_local_candidate",
    p.selected_local_candidate);
  append_json_string_field_if_present(
    &json,
    "libjuice_selected_remote_candidate",
    p.selected_remote_candidate);
  append_json_string_field_if_present(
    &json,
    "libjuice_selected_local_address",
    p.selected_local_address);
  append_json_string_field_if_present(
    &json,
    "libjuice_selected_remote_address",
    p.selected_remote_address);
  append_json_string_field_if_present(
    &json,
    "remote_description_error",
    p.last_remote_description_error);
  json += "}";
  return json;
}

}  // namespace agentd
