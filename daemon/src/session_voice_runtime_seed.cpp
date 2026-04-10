#include "session_voice_runtime_seed.h"

#include <chrono>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::shared_ptr<VoicePeerRuntime> make_voice_peer_runtime_state(
  const VoicePeerRuntimeSeed& seed
) {
  auto st = std::make_shared<VoicePeerRuntime>();
  st->runtime_kind = seed.runtime_kind;
  st->media_engine_kind = seed.media_engine_kind;
  st->media_engine_state = seed.media_engine_state;
  st->session_id = seed.session_id;
  st->broker_session_id = seed.broker_session_id;
  st->broker_url = seed.broker_url;
  st->broker_agent_id = seed.broker_agent_id;
  st->broker_deployment_id = seed.broker_deployment_id;
  st->sender_tag = seed.sender_tag;
  st->tool_path = seed.tool_path;
  st->node_bin = seed.node_bin;
  st->ready_file_path = seed.ready_file_path;
  st->stdout_log_path = seed.stdout_log_path;
  st->stderr_log_path = seed.stderr_log_path;
  st->started_unix_ms = now_unix_ms();
  st->deadline_ms = seed.deadline_ms;
  st->poll_interval_ms = seed.poll_interval_ms;
  st->tone_hz = seed.tone_hz;
  st->media_state_updated_unix_ms =
    seed.media_state_updated_unix_ms > 0 ? seed.media_state_updated_unix_ms : st->started_unix_ms;
  st->media_events_total = seed.media_events_total;
  st->media_remote_offers_seen = seed.media_remote_offers_seen;
  st->media_answers_sent = seed.media_answers_sent;
  st->media_remote_candidates_seen = seed.media_remote_candidates_seen;
  st->media_remote_byes_seen = seed.media_remote_byes_seen;
  st->media_local_byes_sent = seed.media_local_byes_sent;
  st->managed_broker_session = seed.managed_broker_session;
  st->native_media_supported = seed.native_media_supported;
  st->native_media_active = seed.native_media_active;
  st->dtls_identity_ready = seed.dtls_identity_ready;
  st->dtls_handshake_ready = seed.dtls_handshake_ready;
  st->dtls_exporter_ready = seed.dtls_exporter_ready;
  st->srtp_contexts_ready = seed.srtp_contexts_ready;
  st->srtp_inbound_ready = seed.srtp_inbound_ready;
  st->srtp_outbound_ready = seed.srtp_outbound_ready;
  st->dtls_fingerprint_sha256 = seed.dtls_fingerprint_sha256;
  st->dtls_setup_role = seed.dtls_setup_role;
  st->dtls_certificate_subject = seed.dtls_certificate_subject;
  st->dtls_handshake_state = seed.dtls_handshake_state;
  st->dtls_selected_srtp_profile = seed.dtls_selected_srtp_profile;
  st->srtp_last_error = seed.srtp_last_error;
  st->dtls_packets_sent = seed.dtls_packets_sent;
  st->dtls_packets_received = seed.dtls_packets_received;
  st->rtp_packets_received = seed.rtp_packets_received;
  st->rtp_payload_bytes_received = seed.rtp_payload_bytes_received;
  st->rtp_packets_sent = seed.rtp_packets_sent;
  st->rtp_payload_bytes_sent = seed.rtp_payload_bytes_sent;
  st->rtcp_packets_received = seed.rtcp_packets_received;
  st->rtcp_packets_sent = seed.rtcp_packets_sent;
  st->rtcp_payload_bytes_received = seed.rtcp_payload_bytes_received;
  st->rtcp_payload_bytes_sent = seed.rtcp_payload_bytes_sent;
  st->rtcp_sender_reports_sent = seed.rtcp_sender_reports_sent;
  st->rtcp_receiver_reports_sent = seed.rtcp_receiver_reports_sent;
  st->rtcp_receiver_report_blocks_sent = seed.rtcp_receiver_report_blocks_sent;
  st->rtp_last_payload_type = seed.rtp_last_payload_type;
  st->rtp_last_sequence = seed.rtp_last_sequence;
  st->rtp_last_timestamp = seed.rtp_last_timestamp;
  st->rtp_last_ssrc = seed.rtp_last_ssrc;
  st->rtp_last_sent_payload_type = seed.rtp_last_sent_payload_type;
  st->rtp_last_sent_sequence = seed.rtp_last_sent_sequence;
  st->rtp_last_sent_timestamp = seed.rtp_last_sent_timestamp;
  st->rtp_last_sent_ssrc = seed.rtp_last_sent_ssrc;
  st->rtcp_last_packet_type = seed.rtcp_last_packet_type;
  st->rtcp_last_ssrc = seed.rtcp_last_ssrc;
  st->rtcp_last_sent_packet_type = seed.rtcp_last_sent_packet_type;
  st->rtcp_last_sent_ssrc = seed.rtcp_last_sent_ssrc;
  st->rtcp_last_reported_rtp_ssrc = seed.rtcp_last_reported_rtp_ssrc;
  st->rtcp_last_report_fraction_lost = seed.rtcp_last_report_fraction_lost;
  st->rtcp_last_report_cumulative_lost = seed.rtcp_last_report_cumulative_lost;
  st->rtcp_last_report_highest_sequence = seed.rtcp_last_report_highest_sequence;
  st->rtcp_last_report_jitter = seed.rtcp_last_report_jitter;
  st->rtcp_last_report_lsr = seed.rtcp_last_report_lsr;
  st->rtcp_last_report_dlsr = seed.rtcp_last_report_dlsr;
  st->audio_frames_decoded = seed.audio_frames_decoded;
  st->audio_pcm_samples_decoded = seed.audio_pcm_samples_decoded;
  st->audio_pcm_samples_buffered = seed.audio_pcm_samples_buffered;
  st->audio_outbound_frames_sent = seed.audio_outbound_frames_sent;
  st->audio_pcm_samples_submitted_total = seed.audio_pcm_samples_submitted_total;
  st->audio_last_outbound_samples = seed.audio_last_outbound_samples;
  st->audio_outbound_payload_type = seed.audio_outbound_payload_type;
  st->audio_outbound_sample_rate_hz = seed.audio_outbound_sample_rate_hz;
  st->audio_outbound_channels = seed.audio_outbound_channels;
  st->audio_drain_events_total = seed.audio_drain_events_total;
  st->audio_pcm_samples_drained_total = seed.audio_pcm_samples_drained_total;
  st->audio_pcm_samples_owned = seed.audio_pcm_samples_owned;
  st->audio_last_drain_samples = seed.audio_last_drain_samples;
  st->audio_process_events_total = seed.audio_process_events_total;
  st->audio_pcm_samples_processed_total = seed.audio_pcm_samples_processed_total;
  st->audio_last_process_samples = seed.audio_last_process_samples;
  st->audio_last_peak_abs_pcm16 = seed.audio_last_peak_abs_pcm16;
  st->audio_last_rms_pcm16 = seed.audio_last_rms_pcm16;
  st->audio_render_events_total = seed.audio_render_events_total;
  st->audio_pcm_samples_rendered_total = seed.audio_pcm_samples_rendered_total;
  st->audio_render_window_samples = seed.audio_render_window_samples;
  st->audio_last_render_samples = seed.audio_last_render_samples;
  st->audio_playback_enabled = seed.audio_playback_enabled;
  st->audio_playback_stream_open = seed.audio_playback_stream_open;
  st->audio_playback_events_total = seed.audio_playback_events_total;
  st->audio_pcm_samples_played_total = seed.audio_pcm_samples_played_total;
  st->audio_pcm_samples_playback_queued = seed.audio_pcm_samples_playback_queued;
  st->audio_last_playback_samples = seed.audio_last_playback_samples;
  st->audio_last_sample_rate_hz = seed.audio_last_sample_rate_hz;
  st->audio_last_channels = seed.audio_last_channels;
  st->audio_last_frame_samples_per_channel = seed.audio_last_frame_samples_per_channel;
  st->audio_last_codec_name = seed.audio_last_codec_name;
  st->audio_outbound_codec_name = seed.audio_outbound_codec_name;
  st->audio_last_error = seed.audio_last_error;
  st->audio_outbound_last_error = seed.audio_outbound_last_error;
  st->rtcp_last_error = seed.rtcp_last_error;
  st->audio_render_wav_path = seed.audio_render_wav_path;
  st->audio_render_last_error = seed.audio_render_last_error;
  st->audio_playback_device_name = seed.audio_playback_device_name;
  st->audio_playback_last_error = seed.audio_playback_last_error;
  st->native_media_provider = seed.native_media_provider;
  st->ready = seed.ready;
  st->running = seed.running;
  st->pid = seed.pid;
  return st;
}

}  // namespace agentd
