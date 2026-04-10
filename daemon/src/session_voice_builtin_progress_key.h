#pragma once

#include <cstdint>
#include <string>

namespace agentd {

struct BuiltinVoiceAsyncProgressKey {
  std::string libjuice_state;
  std::string dtls_handshake_state;
  std::string dtls_selected_srtp_profile;
  std::string srtp_last_error;
  std::string last_remote_description_error;
  std::string selected_local_candidate;
  std::string selected_remote_candidate;
  std::string selected_local_address;
  std::string selected_remote_address;
  uint64_t offers_seen = 0;
  uint64_t remote_candidates_seen = 0;
  uint64_t local_candidates_observed = 0;
  bool gathering_done = false;
  bool gather_started = false;
  bool remote_description_applied = false;
  bool transport_connectivity_ready = false;
  bool dtls_identity_ready = false;
  bool dtls_handshake_ready = false;
  bool dtls_exporter_ready = false;
  bool srtp_contexts_ready = false;
  bool srtp_inbound_ready = false;
  bool srtp_outbound_ready = false;
  uint64_t rtp_packets_received = 0;
  uint64_t rtp_payload_bytes_received = 0;
  uint64_t rtp_packets_sent = 0;
  uint64_t rtp_payload_bytes_sent = 0;
  uint64_t rtcp_packets_received = 0;
  uint64_t rtcp_packets_sent = 0;
  uint64_t rtcp_payload_bytes_received = 0;
  uint64_t rtcp_payload_bytes_sent = 0;
  uint64_t rtcp_sender_reports_sent = 0;
  uint64_t rtcp_receiver_reports_sent = 0;
  uint64_t rtcp_receiver_report_blocks_sent = 0;
  int64_t rtp_last_payload_type = -1;
  int64_t rtp_last_sequence = -1;
  uint64_t rtp_last_timestamp = 0;
  uint64_t rtp_last_ssrc = 0;
  int64_t rtp_last_sent_payload_type = -1;
  int64_t rtp_last_sent_sequence = -1;
  uint64_t rtp_last_sent_timestamp = 0;
  uint64_t rtp_last_sent_ssrc = 0;
  int64_t rtcp_last_packet_type = -1;
  uint64_t rtcp_last_ssrc = 0;
  int64_t rtcp_last_sent_packet_type = -1;
  uint64_t rtcp_last_sent_ssrc = 0;
  uint64_t rtcp_last_reported_rtp_ssrc = 0;
  int64_t rtcp_last_report_fraction_lost = 0;
  int64_t rtcp_last_report_cumulative_lost = 0;
  uint64_t rtcp_last_report_highest_sequence = 0;
  uint64_t rtcp_last_report_jitter = 0;
  uint64_t rtcp_last_report_lsr = 0;
  uint64_t rtcp_last_report_dlsr = 0;
  uint64_t audio_frames_decoded = 0;
  uint64_t audio_pcm_samples_decoded = 0;
  uint64_t audio_pcm_samples_buffered = 0;
  uint64_t audio_outbound_frames_sent = 0;
  uint64_t audio_pcm_samples_submitted_total = 0;
  uint64_t audio_last_outbound_samples = 0;
  int64_t audio_outbound_payload_type = -1;
  int64_t audio_outbound_sample_rate_hz = 0;
  int64_t audio_outbound_channels = 0;
  std::string audio_outbound_codec_name;
  int64_t audio_last_sample_rate_hz = 0;
  int64_t audio_last_channels = 0;
  int64_t audio_last_frame_samples_per_channel = 0;
  std::string audio_last_codec_name;
  std::string audio_last_error;
  std::string audio_outbound_last_error;
  std::string rtcp_last_error;
};

inline bool operator==(
  const BuiltinVoiceAsyncProgressKey& lhs,
  const BuiltinVoiceAsyncProgressKey& rhs
) {
  return lhs.libjuice_state == rhs.libjuice_state &&
         lhs.dtls_handshake_state == rhs.dtls_handshake_state &&
         lhs.dtls_selected_srtp_profile == rhs.dtls_selected_srtp_profile &&
         lhs.srtp_last_error == rhs.srtp_last_error &&
         lhs.last_remote_description_error == rhs.last_remote_description_error &&
         lhs.selected_local_candidate == rhs.selected_local_candidate &&
         lhs.selected_remote_candidate == rhs.selected_remote_candidate &&
         lhs.selected_local_address == rhs.selected_local_address &&
         lhs.selected_remote_address == rhs.selected_remote_address &&
         lhs.offers_seen == rhs.offers_seen &&
         lhs.remote_candidates_seen == rhs.remote_candidates_seen &&
         lhs.local_candidates_observed == rhs.local_candidates_observed &&
         lhs.gathering_done == rhs.gathering_done &&
         lhs.gather_started == rhs.gather_started &&
         lhs.remote_description_applied == rhs.remote_description_applied &&
         lhs.transport_connectivity_ready == rhs.transport_connectivity_ready &&
         lhs.dtls_identity_ready == rhs.dtls_identity_ready &&
         lhs.dtls_handshake_ready == rhs.dtls_handshake_ready &&
         lhs.dtls_exporter_ready == rhs.dtls_exporter_ready &&
         lhs.srtp_contexts_ready == rhs.srtp_contexts_ready &&
         lhs.srtp_inbound_ready == rhs.srtp_inbound_ready &&
         lhs.srtp_outbound_ready == rhs.srtp_outbound_ready &&
         lhs.rtp_packets_received == rhs.rtp_packets_received &&
         lhs.rtp_payload_bytes_received == rhs.rtp_payload_bytes_received &&
         lhs.rtp_packets_sent == rhs.rtp_packets_sent &&
         lhs.rtp_payload_bytes_sent == rhs.rtp_payload_bytes_sent &&
         lhs.rtcp_packets_received == rhs.rtcp_packets_received &&
         lhs.rtcp_packets_sent == rhs.rtcp_packets_sent &&
         lhs.rtcp_payload_bytes_received == rhs.rtcp_payload_bytes_received &&
         lhs.rtcp_payload_bytes_sent == rhs.rtcp_payload_bytes_sent &&
         lhs.rtcp_sender_reports_sent == rhs.rtcp_sender_reports_sent &&
         lhs.rtcp_receiver_reports_sent == rhs.rtcp_receiver_reports_sent &&
         lhs.rtcp_receiver_report_blocks_sent == rhs.rtcp_receiver_report_blocks_sent &&
         lhs.rtp_last_payload_type == rhs.rtp_last_payload_type &&
         lhs.rtp_last_sequence == rhs.rtp_last_sequence &&
         lhs.rtp_last_timestamp == rhs.rtp_last_timestamp &&
         lhs.rtp_last_ssrc == rhs.rtp_last_ssrc &&
         lhs.rtp_last_sent_payload_type == rhs.rtp_last_sent_payload_type &&
         lhs.rtp_last_sent_sequence == rhs.rtp_last_sent_sequence &&
         lhs.rtp_last_sent_timestamp == rhs.rtp_last_sent_timestamp &&
         lhs.rtp_last_sent_ssrc == rhs.rtp_last_sent_ssrc &&
         lhs.rtcp_last_packet_type == rhs.rtcp_last_packet_type &&
         lhs.rtcp_last_ssrc == rhs.rtcp_last_ssrc &&
         lhs.rtcp_last_sent_packet_type == rhs.rtcp_last_sent_packet_type &&
         lhs.rtcp_last_sent_ssrc == rhs.rtcp_last_sent_ssrc &&
         lhs.rtcp_last_reported_rtp_ssrc == rhs.rtcp_last_reported_rtp_ssrc &&
         lhs.rtcp_last_report_fraction_lost == rhs.rtcp_last_report_fraction_lost &&
         lhs.rtcp_last_report_cumulative_lost == rhs.rtcp_last_report_cumulative_lost &&
         lhs.rtcp_last_report_highest_sequence == rhs.rtcp_last_report_highest_sequence &&
         lhs.rtcp_last_report_jitter == rhs.rtcp_last_report_jitter &&
         lhs.rtcp_last_report_lsr == rhs.rtcp_last_report_lsr &&
         lhs.rtcp_last_report_dlsr == rhs.rtcp_last_report_dlsr &&
         lhs.audio_frames_decoded == rhs.audio_frames_decoded &&
         lhs.audio_pcm_samples_decoded == rhs.audio_pcm_samples_decoded &&
         lhs.audio_pcm_samples_buffered == rhs.audio_pcm_samples_buffered &&
         lhs.audio_outbound_frames_sent == rhs.audio_outbound_frames_sent &&
         lhs.audio_pcm_samples_submitted_total == rhs.audio_pcm_samples_submitted_total &&
         lhs.audio_last_outbound_samples == rhs.audio_last_outbound_samples &&
         lhs.audio_outbound_payload_type == rhs.audio_outbound_payload_type &&
         lhs.audio_outbound_sample_rate_hz == rhs.audio_outbound_sample_rate_hz &&
         lhs.audio_outbound_channels == rhs.audio_outbound_channels &&
         lhs.audio_outbound_codec_name == rhs.audio_outbound_codec_name &&
         lhs.audio_last_sample_rate_hz == rhs.audio_last_sample_rate_hz &&
         lhs.audio_last_channels == rhs.audio_last_channels &&
         lhs.audio_last_frame_samples_per_channel == rhs.audio_last_frame_samples_per_channel &&
         lhs.audio_last_codec_name == rhs.audio_last_codec_name &&
         lhs.audio_last_error == rhs.audio_last_error &&
         lhs.audio_outbound_last_error == rhs.audio_outbound_last_error &&
         lhs.rtcp_last_error == rhs.rtcp_last_error;
}

inline bool operator!=(
  const BuiltinVoiceAsyncProgressKey& lhs,
  const BuiltinVoiceAsyncProgressKey& rhs
) {
  return !(lhs == rhs);
}

}  // namespace agentd
