#include "session_voice_builtin_packet_accounting.h"

namespace agentd {

void copy_builtin_packet_accounting_to_progress(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  BuiltinVoiceAsyncProgressKey* out_progress
) {
  if (!out_progress) return;
  out_progress->rtp_packets_received = accounting.rtp_packets_received;
  out_progress->rtp_payload_bytes_received = accounting.rtp_payload_bytes_received;
  out_progress->rtp_packets_sent = accounting.rtp_packets_sent;
  out_progress->rtp_payload_bytes_sent = accounting.rtp_payload_bytes_sent;
  out_progress->rtcp_packets_received = accounting.rtcp_packets_received;
  out_progress->rtcp_packets_sent = accounting.rtcp_packets_sent;
  out_progress->rtcp_payload_bytes_received = accounting.rtcp_payload_bytes_received;
  out_progress->rtcp_payload_bytes_sent = accounting.rtcp_payload_bytes_sent;
  out_progress->rtcp_sender_reports_sent = accounting.rtcp_sender_reports_sent;
  out_progress->rtcp_receiver_reports_sent = accounting.rtcp_receiver_reports_sent;
  out_progress->rtcp_receiver_report_blocks_sent =
    accounting.rtcp_receiver_report_blocks_sent;
  out_progress->rtp_last_payload_type = accounting.rtp_last_payload_type;
  out_progress->rtp_last_sequence = accounting.rtp_last_sequence;
  out_progress->rtp_last_timestamp = accounting.rtp_last_timestamp;
  out_progress->rtp_last_ssrc = accounting.rtp_last_ssrc;
  out_progress->rtp_last_sent_payload_type = accounting.rtp_last_sent_payload_type;
  out_progress->rtp_last_sent_sequence = accounting.rtp_last_sent_sequence;
  out_progress->rtp_last_sent_timestamp = accounting.rtp_last_sent_timestamp;
  out_progress->rtp_last_sent_ssrc = accounting.rtp_last_sent_ssrc;
  out_progress->rtcp_last_packet_type = accounting.rtcp_last_packet_type;
  out_progress->rtcp_last_ssrc = accounting.rtcp_last_ssrc;
  out_progress->rtcp_last_sent_packet_type = accounting.rtcp_last_sent_packet_type;
  out_progress->rtcp_last_sent_ssrc = accounting.rtcp_last_sent_ssrc;
  out_progress->rtcp_last_reported_rtp_ssrc =
    accounting.rtcp_last_reported_rtp_ssrc;
  out_progress->rtcp_last_report_fraction_lost =
    accounting.rtcp_last_report_fraction_lost;
  out_progress->rtcp_last_report_cumulative_lost =
    accounting.rtcp_last_report_cumulative_lost;
  out_progress->rtcp_last_report_highest_sequence =
    accounting.rtcp_last_report_highest_sequence;
  out_progress->rtcp_last_report_jitter = accounting.rtcp_last_report_jitter;
  out_progress->rtcp_last_report_lsr = accounting.rtcp_last_report_lsr;
  out_progress->rtcp_last_report_dlsr = accounting.rtcp_last_report_dlsr;
  out_progress->rtcp_last_error = accounting.rtcp_last_error;
}

void note_builtin_inbound_rtp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  const ParsedRtpPacketInfo& packet,
  uint32_t clock_rate_hz
) {
  if (!accounting) return;
  note_rtcp_receiver_report_rtp_packet(
    &accounting->inbound_rtcp_report,
    RtcpReceiverReportRtpPacket{
      packet.ssrc,
      packet.sequence,
      packet.timestamp,
      clock_rate_hz,
    });
  accounting->rtp_packets_received += 1;
  accounting->rtp_payload_bytes_received += packet.payload_size;
  accounting->rtp_last_payload_type = packet.payload_type;
  accounting->rtp_last_sequence = packet.sequence;
  accounting->rtp_last_timestamp = packet.timestamp;
  accounting->rtp_last_ssrc = packet.ssrc;
}

void note_builtin_inbound_rtcp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  const ParsedRtcpPacketInfo& packet
) {
  if (!accounting) return;
  accounting->rtcp_packets_received += 1;
  accounting->rtcp_payload_bytes_received += packet.packet_size;
  accounting->rtcp_last_packet_type = packet.packet_type;
  accounting->rtcp_last_ssrc = packet.ssrc;
  if (packet.has_sender_info && packet.sender_report_lsr != 0) {
    note_rtcp_receiver_report_sender_report(
      &accounting->inbound_rtcp_report,
      packet.sender_report_lsr);
  }
  accounting->rtcp_last_error.clear();
}

void note_builtin_outbound_rtp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  int64_t payload_type,
  uint16_t sequence,
  uint32_t timestamp,
  uint32_t ssrc,
  size_t payload_size
) {
  if (!accounting) return;
  accounting->rtp_packets_sent += 1;
  accounting->rtp_payload_bytes_sent += payload_size;
  accounting->rtp_last_sent_payload_type = payload_type;
  accounting->rtp_last_sent_sequence = sequence;
  accounting->rtp_last_sent_timestamp = timestamp;
  accounting->rtp_last_sent_ssrc = ssrc;
}

void note_builtin_outbound_rtcp_sender_report(
  BuiltinRtpRtcpPacketAccounting* accounting,
  uint32_t sender_ssrc,
  size_t payload_size,
  std::chrono::steady_clock::time_point sent_at
) {
  if (!accounting) return;
  accounting->rtcp_packets_sent += 1;
  accounting->rtcp_payload_bytes_sent += payload_size;
  accounting->rtcp_sender_reports_sent += 1;
  accounting->rtcp_last_sent_packet_type = kRtcpPacketTypeSenderReport;
  accounting->rtcp_last_sent_ssrc = sender_ssrc;
  accounting->rtcp_last_error.clear();
  accounting->rtcp_sender_report_sent = true;
  accounting->rtcp_last_sender_report_at = sent_at;
  accounting->rtp_packets_sent_at_last_sender_report =
    accounting->rtp_packets_sent;
}

void note_builtin_outbound_rtcp_receiver_report(
  BuiltinRtpRtcpPacketAccounting* accounting,
  uint32_t sender_ssrc,
  const RtcpReceiverReportBlock& report_block,
  size_t payload_size,
  std::chrono::steady_clock::time_point sent_at
) {
  if (!accounting) return;
  accounting->rtcp_packets_sent += 1;
  accounting->rtcp_payload_bytes_sent += payload_size;
  accounting->rtcp_receiver_reports_sent += 1;
  accounting->rtcp_receiver_report_blocks_sent += 1;
  accounting->rtcp_last_sent_packet_type = kRtcpPacketTypeReceiverReport;
  accounting->rtcp_last_sent_ssrc = sender_ssrc;
  accounting->rtcp_last_reported_rtp_ssrc = report_block.reported_rtp_ssrc;
  accounting->rtcp_last_report_fraction_lost = report_block.fraction_lost;
  accounting->rtcp_last_report_cumulative_lost = report_block.cumulative_lost;
  accounting->rtcp_last_report_highest_sequence =
    report_block.extended_highest_sequence;
  accounting->rtcp_last_report_jitter = report_block.jitter;
  accounting->rtcp_last_report_lsr = report_block.lsr;
  accounting->rtcp_last_report_dlsr = report_block.dlsr;
  accounting->rtcp_last_error.clear();
  mark_rtcp_receiver_report_sent(&accounting->inbound_rtcp_report, sent_at);
}

bool builtin_rtcp_sender_report_due(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  bool transport_ready,
  uint64_t rtp_packet_cadence,
  std::chrono::steady_clock::duration time_cadence,
  std::chrono::steady_clock::time_point now
) {
  if (!transport_ready) return false;
  if (accounting.rtcp_sender_reports_sent == 0) return true;
  if (!accounting.rtcp_sender_report_sent) return true;
  if (rtp_packet_cadence > 0 &&
      accounting.rtp_packets_sent >=
        accounting.rtp_packets_sent_at_last_sender_report + rtp_packet_cadence) {
    return true;
  }
  if (time_cadence <= std::chrono::steady_clock::duration::zero()) {
    return true;
  }
  return now - accounting.rtcp_last_sender_report_at >= time_cadence;
}

bool builtin_rtcp_receiver_report_due(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  bool transport_ready,
  uint64_t rtp_packet_cadence,
  std::chrono::steady_clock::duration time_cadence,
  std::chrono::steady_clock::time_point now
) {
  if (!transport_ready ||
      !rtcp_receiver_report_has_source(accounting.inbound_rtcp_report)) {
    return false;
  }
  if (accounting.rtcp_receiver_reports_sent == 0) return true;
  if (rtp_packet_cadence > 0 &&
      accounting.inbound_rtcp_report.packets_since_last_report >=
        rtp_packet_cadence) {
    return true;
  }
  if (!accounting.inbound_rtcp_report.receiver_report_sent) return true;
  if (time_cadence <= std::chrono::steady_clock::duration::zero()) {
    return true;
  }
  return now - accounting.inbound_rtcp_report.last_receiver_report_at >=
         time_cadence;
}

}  // namespace agentd
