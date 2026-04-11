#pragma once

#include "session_voice_builtin_progress_key.h"
#include "session_voice_dtls_srtp_util.h"
#include "session_voice_rtcp_report.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace agentd {

struct BuiltinRtpRtcpPacketAccounting {
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
  std::string rtcp_last_error;
  bool rtcp_sender_report_sent = false;
  std::chrono::steady_clock::time_point rtcp_last_sender_report_at;
  uint64_t rtp_packets_sent_at_last_sender_report = 0;
  RtcpReceiverReportTracker inbound_rtcp_report;
};

void copy_builtin_packet_accounting_to_progress(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  BuiltinVoiceAsyncProgressKey* out_progress);

void note_builtin_inbound_rtp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  const ParsedRtpPacketInfo& packet,
  uint32_t clock_rate_hz);

void note_builtin_inbound_rtcp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  const ParsedRtcpPacketInfo& packet);

void note_builtin_outbound_rtp_packet(
  BuiltinRtpRtcpPacketAccounting* accounting,
  int64_t payload_type,
  uint16_t sequence,
  uint32_t timestamp,
  uint32_t ssrc,
  size_t payload_size);

void note_builtin_outbound_rtcp_sender_report(
  BuiltinRtpRtcpPacketAccounting* accounting,
  uint32_t sender_ssrc,
  size_t payload_size,
  std::chrono::steady_clock::time_point sent_at =
    std::chrono::steady_clock::now());

void note_builtin_outbound_rtcp_receiver_report(
  BuiltinRtpRtcpPacketAccounting* accounting,
  uint32_t sender_ssrc,
  const RtcpReceiverReportBlock& report_block,
  size_t payload_size,
  std::chrono::steady_clock::time_point sent_at =
    std::chrono::steady_clock::now());

bool builtin_rtcp_sender_report_due(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  bool transport_ready,
  uint64_t rtp_packet_cadence,
  std::chrono::steady_clock::duration time_cadence,
  std::chrono::steady_clock::time_point now =
    std::chrono::steady_clock::now());

bool builtin_rtcp_receiver_report_due(
  const BuiltinRtpRtcpPacketAccounting& accounting,
  bool transport_ready,
  uint64_t rtp_packet_cadence,
  std::chrono::steady_clock::duration time_cadence,
  std::chrono::steady_clock::time_point now =
    std::chrono::steady_clock::now());

}  // namespace agentd
