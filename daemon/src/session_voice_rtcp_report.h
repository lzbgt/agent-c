#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace agentd {

inline constexpr uint8_t kRtcpPacketTypeSenderReport = 200;
inline constexpr uint8_t kRtcpPacketTypeReceiverReport = 201;
inline constexpr uint8_t kRtcpPacketTypeSourceDescription = 202;
inline constexpr size_t kRtcpSenderReportBytes = 28;
inline constexpr size_t kRtcpReceiverReportWithBlockBytes = 32;

struct RtcpSenderReportInput {
  uint32_t sender_ssrc = 0;
  uint32_t rtp_timestamp = 0;
  uint64_t rtp_packets_sent = 0;
  uint64_t rtp_payload_bytes_sent = 0;
};

struct RtcpReceiverReportRtpPacket {
  uint32_t ssrc = 0;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t clock_rate_hz = 0;
};

struct RtcpReceiverReportBlock {
  uint32_t reported_rtp_ssrc = 0;
  uint8_t fraction_lost = 0;
  int64_t cumulative_lost = 0;
  uint32_t extended_highest_sequence = 0;
  uint32_t jitter = 0;
  uint32_t lsr = 0;
  uint32_t dlsr = 0;
};

struct RtcpReceiverReportTracker {
  bool source_initialized = false;
  uint32_t source_ssrc = 0;
  uint16_t base_sequence = 0;
  uint16_t max_sequence = 0;
  uint32_t sequence_cycles = 0;
  uint32_t extended_highest_sequence = 0;
  uint64_t source_packets_received = 0;
  uint64_t prior_expected = 0;
  uint64_t prior_received = 0;
  uint64_t packets_since_last_report = 0;
  bool transit_initialized = false;
  int64_t last_transit = 0;
  double jitter = 0.0;
  uint32_t clock_rate_hz = 0;
  bool remote_sender_report_seen = false;
  uint32_t remote_sender_report_lsr = 0;
  std::chrono::steady_clock::time_point remote_sender_report_received_at;
  bool receiver_report_sent = false;
  std::chrono::steady_clock::time_point last_receiver_report_at;
};

using RtcpSenderReportPacket =
  std::array<unsigned char, kRtcpSenderReportBytes>;
using RtcpReceiverReportPacket =
  std::array<unsigned char, kRtcpReceiverReportWithBlockBytes>;

std::pair<uint32_t, uint32_t> current_ntp_timestamp_words();

void note_rtcp_receiver_report_rtp_packet(
  RtcpReceiverReportTracker* tracker,
  const RtcpReceiverReportRtpPacket& packet,
  std::chrono::steady_clock::time_point arrival =
    std::chrono::steady_clock::now());

void note_rtcp_receiver_report_sender_report(
  RtcpReceiverReportTracker* tracker,
  uint32_t sender_report_lsr,
  std::chrono::steady_clock::time_point received_at =
    std::chrono::steady_clock::now());

bool rtcp_receiver_report_has_source(const RtcpReceiverReportTracker& tracker);
uint64_t rtcp_receiver_report_expected_packets(
  const RtcpReceiverReportTracker& tracker);
int64_t rtcp_receiver_report_cumulative_lost(
  const RtcpReceiverReportTracker& tracker);
uint8_t rtcp_receiver_report_fraction_lost_since_last_report(
  const RtcpReceiverReportTracker& tracker);

RtcpReceiverReportBlock snapshot_rtcp_receiver_report_block(
  const RtcpReceiverReportTracker& tracker,
  std::chrono::steady_clock::time_point now =
    std::chrono::steady_clock::now());

RtcpSenderReportPacket build_rtcp_sender_report(
  const RtcpSenderReportInput& input);
RtcpReceiverReportPacket build_rtcp_receiver_report(
  uint32_t sender_ssrc,
  const RtcpReceiverReportBlock& block);
std::vector<unsigned char> build_rtcp_sdes_cname(
  uint32_t sender_ssrc,
  const std::string& cname);
std::vector<unsigned char> build_rtcp_sender_report_compound(
  const RtcpSenderReportInput& input,
  const std::string& cname);
std::vector<unsigned char> build_rtcp_receiver_report_compound(
  uint32_t sender_ssrc,
  const RtcpReceiverReportBlock& block,
  const std::string& cname);

void mark_rtcp_receiver_report_sent(
  RtcpReceiverReportTracker* tracker,
  std::chrono::steady_clock::time_point sent_at =
    std::chrono::steady_clock::now());

}  // namespace agentd
