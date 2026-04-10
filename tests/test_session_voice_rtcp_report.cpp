#include "session_voice_rtcp_report.h"

#include <cassert>
#include <chrono>
#include <cstdint>

namespace {

uint16_t read_u16_be(const unsigned char* in) {
  return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) |
                               static_cast<uint16_t>(in[1]));
}

uint32_t read_u32_be(const unsigned char* in) {
  return (static_cast<uint32_t>(in[0]) << 24) |
         (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) |
         static_cast<uint32_t>(in[3]);
}

int32_t read_i24_be(const unsigned char* in) {
  const uint32_t encoded =
    (static_cast<uint32_t>(in[0]) << 16) |
    (static_cast<uint32_t>(in[1]) << 8) |
    static_cast<uint32_t>(in[2]);
  if ((encoded & 0x00800000u) == 0) return static_cast<int32_t>(encoded);
  return static_cast<int32_t>(encoded | 0xFF000000u);
}

void test_sender_report_packet_shape() {
  const auto packet = agentd::build_rtcp_sender_report(agentd::RtcpSenderReportInput{
    0x11223344u,
    0x01020304u,
    7,
    160,
  });

  assert(packet[0] == 0x80u);
  assert(packet[1] == agentd::kRtcpPacketTypeSenderReport);
  assert(read_u16_be(packet.data() + 2) == 6);
  assert(read_u32_be(packet.data() + 4) == 0x11223344u);
  assert(read_u32_be(packet.data() + 16) == 0x01020304u);
  assert(read_u32_be(packet.data() + 20) == 7u);
  assert(read_u32_be(packet.data() + 24) == 160u);
}

void test_receiver_report_tracks_rollover_and_dlsr() {
  using namespace std::chrono;
  const auto base = steady_clock::time_point(microseconds(1000000));

  agentd::RtcpReceiverReportTracker tracker;
  agentd::note_rtcp_receiver_report_sender_report(&tracker, 0x12345678u, base);
  agentd::note_rtcp_receiver_report_rtp_packet(
    &tracker,
    agentd::RtcpReceiverReportRtpPacket{0xCAFEBABEu, 65534, 8000, 8000},
    base);
  agentd::note_rtcp_receiver_report_sender_report(&tracker, 0x12345678u, base);
  agentd::note_rtcp_receiver_report_rtp_packet(
    &tracker,
    agentd::RtcpReceiverReportRtpPacket{0xCAFEBABEu, 65535, 8160, 8000},
    base + milliseconds(20));
  agentd::note_rtcp_receiver_report_rtp_packet(
    &tracker,
    agentd::RtcpReceiverReportRtpPacket{0xCAFEBABEu, 0, 8320, 8000},
    base + milliseconds(40));

  assert(agentd::rtcp_receiver_report_has_source(tracker));
  assert(agentd::rtcp_receiver_report_expected_packets(tracker) == 3);
  assert(agentd::rtcp_receiver_report_cumulative_lost(tracker) == 0);
  assert(agentd::rtcp_receiver_report_fraction_lost_since_last_report(tracker) == 0);
  assert(tracker.extended_highest_sequence == 0x00010000u);
  assert(tracker.packets_since_last_report == 3);

  const auto block =
    agentd::snapshot_rtcp_receiver_report_block(tracker, base + seconds(1));
  assert(block.reported_rtp_ssrc == 0xCAFEBABEu);
  assert(block.fraction_lost == 0);
  assert(block.cumulative_lost == 0);
  assert(block.extended_highest_sequence == 0x00010000u);
  assert(block.jitter == 0);
  assert(block.lsr == 0x12345678u);
  assert(block.dlsr == 65536u);

  const auto packet = agentd::build_rtcp_receiver_report(0xAABBCCDDu, block);
  assert(packet[0] == 0x81u);
  assert(packet[1] == agentd::kRtcpPacketTypeReceiverReport);
  assert(read_u16_be(packet.data() + 2) == 7);
  assert(read_u32_be(packet.data() + 4) == 0xAABBCCDDu);
  assert(read_u32_be(packet.data() + 8) == 0xCAFEBABEu);
  assert(packet[12] == 0);
  assert(read_i24_be(packet.data() + 13) == 0);
  assert(read_u32_be(packet.data() + 16) == 0x00010000u);
  assert(read_u32_be(packet.data() + 20) == 0u);
  assert(read_u32_be(packet.data() + 24) == 0x12345678u);
  assert(read_u32_be(packet.data() + 28) == 65536u);

  agentd::mark_rtcp_receiver_report_sent(&tracker, base + seconds(1));
  assert(tracker.receiver_report_sent);
  assert(tracker.packets_since_last_report == 0);
  assert(tracker.prior_expected == 3);
  assert(tracker.prior_received == 3);
}

void test_receiver_report_interval_loss_after_mark() {
  using namespace std::chrono;
  const auto base = steady_clock::time_point(microseconds(2000000));

  agentd::RtcpReceiverReportTracker tracker;
  for (uint16_t sequence : {static_cast<uint16_t>(65534), static_cast<uint16_t>(65535), static_cast<uint16_t>(0)}) {
    agentd::note_rtcp_receiver_report_rtp_packet(
      &tracker,
      agentd::RtcpReceiverReportRtpPacket{
        0x01020304u,
        sequence,
        static_cast<uint32_t>(8000 + (sequence == 0 ? 320 : (sequence - 65534) * 160)),
        8000,
      },
      base);
  }
  agentd::mark_rtcp_receiver_report_sent(&tracker, base);

  agentd::note_rtcp_receiver_report_rtp_packet(
    &tracker,
    agentd::RtcpReceiverReportRtpPacket{0x01020304u, 2, 8480, 8000},
    base + milliseconds(60));
  agentd::note_rtcp_receiver_report_rtp_packet(
    &tracker,
    agentd::RtcpReceiverReportRtpPacket{0x01020304u, 4, 8800, 8000},
    base + milliseconds(100));

  const auto block = agentd::snapshot_rtcp_receiver_report_block(tracker, base + milliseconds(100));
  assert(block.extended_highest_sequence == 0x00010004u);
  assert(block.cumulative_lost == 2);
  assert(block.fraction_lost == 128);

  const auto packet = agentd::build_rtcp_receiver_report(0x0A0B0C0Du, block);
  assert(packet[12] == 128);
  assert(read_i24_be(packet.data() + 13) == 2);
  assert(read_u32_be(packet.data() + 16) == 0x00010004u);
}

}  // namespace

int main() {
  test_sender_report_packet_shape();
  test_receiver_report_tracks_rollover_and_dlsr();
  test_receiver_report_interval_loss_after_mark();
  return 0;
}
