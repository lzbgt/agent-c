#include "session_voice_rtcp_report.h"

#include <algorithm>
#include <limits>

namespace agentd {
namespace {

constexpr uint64_t kUnixToNtpEpochSeconds = 2208988800ULL;

void write_u16_be(unsigned char* out, uint16_t value) {
  out[0] = static_cast<unsigned char>((value >> 8) & 0xFF);
  out[1] = static_cast<unsigned char>(value & 0xFF);
}

void write_u32_be(unsigned char* out, uint32_t value) {
  out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
  out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
  out[2] = static_cast<unsigned char>((value >> 8) & 0xFF);
  out[3] = static_cast<unsigned char>(value & 0xFF);
}

void write_i24_be(unsigned char* out, int64_t value) {
  const int64_t clamped =
    std::max<int64_t>(-8388608, std::min<int64_t>(8388607, value));
  const uint32_t encoded = static_cast<uint32_t>(clamped) & 0x00FFFFFFu;
  out[0] = static_cast<unsigned char>((encoded >> 16) & 0xFF);
  out[1] = static_cast<unsigned char>((encoded >> 8) & 0xFF);
  out[2] = static_cast<unsigned char>(encoded & 0xFF);
}

uint64_t steady_microseconds(std::chrono::steady_clock::time_point value) {
  using namespace std::chrono;
  return static_cast<uint64_t>(
    duration_cast<microseconds>(value.time_since_epoch()).count());
}

uint32_t elapsed_dlsr_units(
  std::chrono::steady_clock::time_point since,
  std::chrono::steady_clock::time_point now) {
  using namespace std::chrono;
  const auto micros = duration_cast<microseconds>(now - since).count();
  if (micros <= 0) return 0;
  const uint64_t units =
    (static_cast<uint64_t>(micros) * static_cast<uint64_t>(65536)) /
    static_cast<uint64_t>(1000000);
  return static_cast<uint32_t>(
    std::min<uint64_t>(units, std::numeric_limits<uint32_t>::max()));
}

uint32_t rtp_arrival_units(uint64_t steady_us, uint32_t clock_rate_hz) {
  if (clock_rate_hz == 0) return 0;
  const long double units =
    (static_cast<long double>(steady_us) *
     static_cast<long double>(clock_rate_hz)) /
    static_cast<long double>(1000000.0L);
  return static_cast<uint32_t>(static_cast<uint64_t>(units) & 0xFFFFFFFFu);
}

uint32_t report_jitter_sample(const RtcpReceiverReportTracker& tracker) {
  if (tracker.jitter <= 0.0) return 0;
  const double bounded = std::min<double>(
    tracker.jitter,
    static_cast<double>(std::numeric_limits<uint32_t>::max()));
  return static_cast<uint32_t>(bounded + 0.5);
}

int64_t clamped_report_loss(int64_t value) {
  return std::max<int64_t>(-8388608, std::min<int64_t>(8388607, value));
}

}  // namespace

std::pair<uint32_t, uint32_t> current_ntp_timestamp_words() {
  using namespace std::chrono;
  const auto now = system_clock::now().time_since_epoch();
  const auto seconds_part = duration_cast<seconds>(now);
  const auto nanos_part = duration_cast<nanoseconds>(now - seconds_part);
  const uint64_t ntp_seconds =
    static_cast<uint64_t>(seconds_part.count()) + kUnixToNtpEpochSeconds;
  const uint64_t ntp_fraction =
    (static_cast<uint64_t>(nanos_part.count()) << 32) / 1000000000ULL;
  return {
    static_cast<uint32_t>(ntp_seconds & 0xFFFFFFFFu),
    static_cast<uint32_t>(ntp_fraction & 0xFFFFFFFFu),
  };
}

void note_rtcp_receiver_report_rtp_packet(
  RtcpReceiverReportTracker* tracker,
  const RtcpReceiverReportRtpPacket& packet,
  std::chrono::steady_clock::time_point arrival) {
  if (!tracker) return;
  const uint32_t clock_rate_hz = packet.clock_rate_hz > 0 ? packet.clock_rate_hz : 8000;

  if (!tracker->source_initialized || tracker->source_ssrc != packet.ssrc) {
    tracker->source_initialized = true;
    tracker->source_ssrc = packet.ssrc;
    tracker->clock_rate_hz = clock_rate_hz;
    tracker->base_sequence = packet.sequence;
    tracker->max_sequence = packet.sequence;
    tracker->sequence_cycles = 0;
    tracker->extended_highest_sequence = packet.sequence;
    tracker->source_packets_received = 0;
    tracker->prior_expected = 0;
    tracker->prior_received = 0;
    tracker->packets_since_last_report = 0;
    tracker->transit_initialized = false;
    tracker->last_transit = 0;
    tracker->jitter = 0.0;
    tracker->remote_sender_report_seen = false;
    tracker->remote_sender_report_lsr = 0;
    tracker->receiver_report_sent = false;
  } else if (packet.sequence < tracker->max_sequence &&
             static_cast<uint16_t>(tracker->max_sequence - packet.sequence) > 0x8000u) {
    tracker->sequence_cycles += 0x10000u;
    tracker->max_sequence = packet.sequence;
  } else if (packet.sequence > tracker->max_sequence) {
    tracker->max_sequence = packet.sequence;
  }

  const uint32_t extended_sequence =
    tracker->sequence_cycles + static_cast<uint32_t>(packet.sequence);
  tracker->extended_highest_sequence =
    std::max(tracker->extended_highest_sequence, extended_sequence);
  tracker->source_packets_received += 1;
  tracker->packets_since_last_report += 1;

  const uint64_t arrival_us = steady_microseconds(arrival);
  const uint32_t arrival_rtp_units = rtp_arrival_units(arrival_us, clock_rate_hz);
  const int64_t transit =
    static_cast<int64_t>(arrival_rtp_units) - static_cast<int64_t>(packet.timestamp);
  if (tracker->transit_initialized) {
    int64_t delta = transit - tracker->last_transit;
    if (delta < 0) delta = -delta;
    tracker->jitter += (static_cast<double>(delta) - tracker->jitter) / 16.0;
  } else {
    tracker->transit_initialized = true;
  }
  tracker->last_transit = transit;
}

void note_rtcp_receiver_report_sender_report(
  RtcpReceiverReportTracker* tracker,
  uint32_t sender_report_lsr,
  std::chrono::steady_clock::time_point received_at) {
  if (!tracker || sender_report_lsr == 0) return;
  tracker->remote_sender_report_seen = true;
  tracker->remote_sender_report_lsr = sender_report_lsr;
  tracker->remote_sender_report_received_at = received_at;
}

bool rtcp_receiver_report_has_source(const RtcpReceiverReportTracker& tracker) {
  return tracker.source_initialized;
}

uint64_t rtcp_receiver_report_expected_packets(
  const RtcpReceiverReportTracker& tracker) {
  if (!tracker.source_initialized) return 0;
  if (tracker.extended_highest_sequence < tracker.base_sequence) {
    return tracker.source_packets_received;
  }
  return static_cast<uint64_t>(
    tracker.extended_highest_sequence -
    static_cast<uint32_t>(tracker.base_sequence) + 1u);
}

int64_t rtcp_receiver_report_cumulative_lost(
  const RtcpReceiverReportTracker& tracker) {
  return static_cast<int64_t>(rtcp_receiver_report_expected_packets(tracker)) -
         static_cast<int64_t>(tracker.source_packets_received);
}

uint8_t rtcp_receiver_report_fraction_lost_since_last_report(
  const RtcpReceiverReportTracker& tracker) {
  const uint64_t expected = rtcp_receiver_report_expected_packets(tracker);
  const uint64_t expected_interval =
    expected > tracker.prior_expected ? expected - tracker.prior_expected : 0;
  const uint64_t received_interval =
    tracker.source_packets_received > tracker.prior_received
      ? tracker.source_packets_received - tracker.prior_received
      : 0;
  if (expected_interval == 0 || received_interval >= expected_interval) return 0;
  const uint64_t lost_interval = expected_interval - received_interval;
  return static_cast<uint8_t>(
    std::min<uint64_t>((lost_interval * 256u) / expected_interval, 255));
}

RtcpReceiverReportBlock snapshot_rtcp_receiver_report_block(
  const RtcpReceiverReportTracker& tracker,
  std::chrono::steady_clock::time_point now) {
  RtcpReceiverReportBlock block;
  if (!tracker.source_initialized) return block;
  block.reported_rtp_ssrc = tracker.source_ssrc;
  block.fraction_lost = rtcp_receiver_report_fraction_lost_since_last_report(tracker);
  block.cumulative_lost = clamped_report_loss(
    rtcp_receiver_report_cumulative_lost(tracker));
  block.extended_highest_sequence = tracker.extended_highest_sequence;
  block.jitter = report_jitter_sample(tracker);
  block.lsr = tracker.remote_sender_report_seen ? tracker.remote_sender_report_lsr : 0;
  block.dlsr = tracker.remote_sender_report_seen
                 ? elapsed_dlsr_units(tracker.remote_sender_report_received_at, now)
                 : 0;
  return block;
}

RtcpSenderReportPacket build_rtcp_sender_report(
  const RtcpSenderReportInput& input) {
  RtcpSenderReportPacket out{};
  const auto ntp = current_ntp_timestamp_words();
  out[0] = 0x80u;
  out[1] = kRtcpPacketTypeSenderReport;
  write_u16_be(out.data() + 2, 6);
  write_u32_be(out.data() + 4, input.sender_ssrc);
  write_u32_be(out.data() + 8, ntp.first);
  write_u32_be(out.data() + 12, ntp.second);
  write_u32_be(out.data() + 16, input.rtp_timestamp);
  write_u32_be(out.data() + 20, static_cast<uint32_t>(input.rtp_packets_sent));
  write_u32_be(out.data() + 24, static_cast<uint32_t>(input.rtp_payload_bytes_sent));
  return out;
}

RtcpReceiverReportPacket build_rtcp_receiver_report(
  uint32_t sender_ssrc,
  const RtcpReceiverReportBlock& block) {
  RtcpReceiverReportPacket out{};
  out[0] = 0x81u;
  out[1] = kRtcpPacketTypeReceiverReport;
  write_u16_be(out.data() + 2, 7);
  write_u32_be(out.data() + 4, sender_ssrc);
  write_u32_be(out.data() + 8, block.reported_rtp_ssrc);
  out[12] = block.fraction_lost;
  write_i24_be(out.data() + 13, block.cumulative_lost);
  write_u32_be(out.data() + 16, block.extended_highest_sequence);
  write_u32_be(out.data() + 20, block.jitter);
  write_u32_be(out.data() + 24, block.lsr);
  write_u32_be(out.data() + 28, block.dlsr);
  return out;
}

void mark_rtcp_receiver_report_sent(
  RtcpReceiverReportTracker* tracker,
  std::chrono::steady_clock::time_point sent_at) {
  if (!tracker) return;
  tracker->prior_expected = rtcp_receiver_report_expected_packets(*tracker);
  tracker->prior_received = tracker->source_packets_received;
  tracker->packets_since_last_report = 0;
  tracker->receiver_report_sent = true;
  tracker->last_receiver_report_at = sent_at;
}

}  // namespace agentd
