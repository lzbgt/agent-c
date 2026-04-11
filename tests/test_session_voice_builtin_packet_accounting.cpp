#include "session_voice_builtin_packet_accounting.h"

#include <cassert>
#include <chrono>

namespace {

void test_inbound_rtp_updates_counters_and_receiver_report_tracker() {
  agentd::BuiltinRtpRtcpPacketAccounting accounting;
  agentd::ParsedRtpPacketInfo packet;
  packet.payload_type = 111;
  packet.sequence = 42;
  packet.timestamp = 960;
  packet.ssrc = 0x12345678u;
  packet.payload_size = 120;

  agentd::note_builtin_inbound_rtp_packet(&accounting, packet, 48000);

  assert(accounting.rtp_packets_received == 1);
  assert(accounting.rtp_payload_bytes_received == 120);
  assert(accounting.rtp_last_payload_type == 111);
  assert(accounting.rtp_last_sequence == 42);
  assert(accounting.rtp_last_timestamp == 960);
  assert(accounting.rtp_last_ssrc == 0x12345678u);
  assert(accounting.inbound_rtcp_report.source_initialized);
  assert(accounting.inbound_rtcp_report.source_ssrc == 0x12345678u);
  assert(accounting.inbound_rtcp_report.clock_rate_hz == 48000);
}

void test_inbound_rtcp_updates_sender_report_metadata() {
  agentd::BuiltinRtpRtcpPacketAccounting accounting;
  accounting.rtcp_last_error = "previous";

  agentd::ParsedRtcpPacketInfo packet;
  packet.packet_type = agentd::kRtcpPacketTypeSenderReport;
  packet.ssrc = 0xAABBCCDDu;
  packet.packet_size = 28;
  packet.has_sender_info = true;
  packet.sender_report_lsr = 0x01020304u;

  agentd::note_builtin_inbound_rtcp_packet(&accounting, packet);

  assert(accounting.rtcp_packets_received == 1);
  assert(accounting.rtcp_payload_bytes_received == 28);
  assert(accounting.rtcp_last_packet_type == agentd::kRtcpPacketTypeSenderReport);
  assert(accounting.rtcp_last_ssrc == 0xAABBCCDDu);
  assert(accounting.rtcp_last_error.empty());
  assert(accounting.inbound_rtcp_report.remote_sender_report_seen);
  assert(accounting.inbound_rtcp_report.remote_sender_report_lsr == 0x01020304u);
}

void test_outbound_rtp_and_sender_report_cadence() {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point now = Clock::time_point{} + std::chrono::seconds(10);

  agentd::BuiltinRtpRtcpPacketAccounting accounting;
  assert(!agentd::builtin_rtcp_sender_report_due(
    accounting,
    false,
    50,
    std::chrono::seconds(5),
    now));
  assert(agentd::builtin_rtcp_sender_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now));

  agentd::note_builtin_outbound_rtp_packet(
    &accounting,
    111,
    7,
    960,
    0xA6E17D01u,
    80);
  assert(accounting.rtp_packets_sent == 1);
  assert(accounting.rtp_payload_bytes_sent == 80);
  assert(accounting.rtp_last_sent_payload_type == 111);
  assert(accounting.rtp_last_sent_sequence == 7);
  assert(accounting.rtp_last_sent_timestamp == 960);
  assert(accounting.rtp_last_sent_ssrc == 0xA6E17D01u);

  agentd::note_builtin_outbound_rtcp_sender_report(
    &accounting,
    0xA6E17D01u,
    28,
    now);
  assert(accounting.rtcp_packets_sent == 1);
  assert(accounting.rtcp_payload_bytes_sent == 28);
  assert(accounting.rtcp_sender_reports_sent == 1);
  assert(accounting.rtcp_last_sent_packet_type == agentd::kRtcpPacketTypeSenderReport);
  assert(accounting.rtcp_last_sent_ssrc == 0xA6E17D01u);
  assert(accounting.rtcp_sender_report_sent);
  assert(accounting.rtp_packets_sent_at_last_sender_report == 1);
  assert(!agentd::builtin_rtcp_sender_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now + std::chrono::seconds(1)));
  assert(agentd::builtin_rtcp_sender_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now + std::chrono::seconds(5)));

  for (uint16_t i = 0; i < 50; ++i) {
    agentd::note_builtin_outbound_rtp_packet(
      &accounting,
      111,
      static_cast<uint16_t>(8 + i),
      1920 + i,
      0xA6E17D01u,
      80);
  }
  assert(agentd::builtin_rtcp_sender_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now + std::chrono::seconds(1)));
}

void test_outbound_receiver_report_updates_report_block_fields() {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point now = Clock::time_point{} + std::chrono::seconds(20);

  agentd::BuiltinRtpRtcpPacketAccounting accounting;
  agentd::ParsedRtpPacketInfo packet;
  packet.sequence = 10;
  packet.timestamp = 160;
  packet.ssrc = 0x01010101u;
  packet.payload_size = 40;
  agentd::note_builtin_inbound_rtp_packet(&accounting, packet, 8000);

  assert(agentd::builtin_rtcp_receiver_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now));

  agentd::RtcpReceiverReportBlock block;
  block.reported_rtp_ssrc = 0x01010101u;
  block.fraction_lost = 2;
  block.cumulative_lost = -1;
  block.extended_highest_sequence = 10;
  block.jitter = 3;
  block.lsr = 4;
  block.dlsr = 5;

  agentd::note_builtin_outbound_rtcp_receiver_report(
    &accounting,
    0xA6E17D01u,
    block,
    32,
    now);

  assert(accounting.rtcp_packets_sent == 1);
  assert(accounting.rtcp_payload_bytes_sent == 32);
  assert(accounting.rtcp_receiver_reports_sent == 1);
  assert(accounting.rtcp_receiver_report_blocks_sent == 1);
  assert(accounting.rtcp_last_sent_packet_type == agentd::kRtcpPacketTypeReceiverReport);
  assert(accounting.rtcp_last_sent_ssrc == 0xA6E17D01u);
  assert(accounting.rtcp_last_reported_rtp_ssrc == 0x01010101u);
  assert(accounting.rtcp_last_report_fraction_lost == 2);
  assert(accounting.rtcp_last_report_cumulative_lost == -1);
  assert(accounting.rtcp_last_report_highest_sequence == 10);
  assert(accounting.rtcp_last_report_jitter == 3);
  assert(accounting.rtcp_last_report_lsr == 4);
  assert(accounting.rtcp_last_report_dlsr == 5);
  assert(accounting.inbound_rtcp_report.receiver_report_sent);
  assert(!agentd::builtin_rtcp_receiver_report_due(
    accounting,
    true,
    50,
    std::chrono::seconds(5),
    now + std::chrono::seconds(1)));
}

void test_copy_to_progress_key() {
  agentd::BuiltinRtpRtcpPacketAccounting accounting;
  accounting.rtp_packets_received = 1;
  accounting.rtp_payload_bytes_received = 2;
  accounting.rtp_packets_sent = 3;
  accounting.rtp_payload_bytes_sent = 4;
  accounting.rtcp_packets_received = 5;
  accounting.rtcp_packets_sent = 6;
  accounting.rtcp_payload_bytes_received = 7;
  accounting.rtcp_payload_bytes_sent = 8;
  accounting.rtcp_sender_reports_sent = 9;
  accounting.rtcp_receiver_reports_sent = 10;
  accounting.rtcp_receiver_report_blocks_sent = 11;
  accounting.rtp_last_payload_type = 12;
  accounting.rtp_last_sequence = 13;
  accounting.rtp_last_timestamp = 14;
  accounting.rtp_last_ssrc = 15;
  accounting.rtp_last_sent_payload_type = 16;
  accounting.rtp_last_sent_sequence = 17;
  accounting.rtp_last_sent_timestamp = 18;
  accounting.rtp_last_sent_ssrc = 19;
  accounting.rtcp_last_packet_type = 20;
  accounting.rtcp_last_ssrc = 21;
  accounting.rtcp_last_sent_packet_type = 22;
  accounting.rtcp_last_sent_ssrc = 23;
  accounting.rtcp_last_reported_rtp_ssrc = 24;
  accounting.rtcp_last_report_fraction_lost = 25;
  accounting.rtcp_last_report_cumulative_lost = -26;
  accounting.rtcp_last_report_highest_sequence = 27;
  accounting.rtcp_last_report_jitter = 28;
  accounting.rtcp_last_report_lsr = 29;
  accounting.rtcp_last_report_dlsr = 30;
  accounting.rtcp_last_error = "rtcp";

  agentd::BuiltinVoiceAsyncProgressKey progress;
  agentd::copy_builtin_packet_accounting_to_progress(accounting, &progress);

  assert(progress.rtp_packets_received == 1);
  assert(progress.rtp_payload_bytes_received == 2);
  assert(progress.rtp_packets_sent == 3);
  assert(progress.rtp_payload_bytes_sent == 4);
  assert(progress.rtcp_packets_received == 5);
  assert(progress.rtcp_packets_sent == 6);
  assert(progress.rtcp_payload_bytes_received == 7);
  assert(progress.rtcp_payload_bytes_sent == 8);
  assert(progress.rtcp_sender_reports_sent == 9);
  assert(progress.rtcp_receiver_reports_sent == 10);
  assert(progress.rtcp_receiver_report_blocks_sent == 11);
  assert(progress.rtp_last_payload_type == 12);
  assert(progress.rtp_last_sequence == 13);
  assert(progress.rtp_last_timestamp == 14);
  assert(progress.rtp_last_ssrc == 15);
  assert(progress.rtp_last_sent_payload_type == 16);
  assert(progress.rtp_last_sent_sequence == 17);
  assert(progress.rtp_last_sent_timestamp == 18);
  assert(progress.rtp_last_sent_ssrc == 19);
  assert(progress.rtcp_last_packet_type == 20);
  assert(progress.rtcp_last_ssrc == 21);
  assert(progress.rtcp_last_sent_packet_type == 22);
  assert(progress.rtcp_last_sent_ssrc == 23);
  assert(progress.rtcp_last_reported_rtp_ssrc == 24);
  assert(progress.rtcp_last_report_fraction_lost == 25);
  assert(progress.rtcp_last_report_cumulative_lost == -26);
  assert(progress.rtcp_last_report_highest_sequence == 27);
  assert(progress.rtcp_last_report_jitter == 28);
  assert(progress.rtcp_last_report_lsr == 29);
  assert(progress.rtcp_last_report_dlsr == 30);
  assert(progress.rtcp_last_error == "rtcp");
}

}  // namespace

int main() {
  test_inbound_rtp_updates_counters_and_receiver_report_tracker();
  test_inbound_rtcp_updates_sender_report_metadata();
  test_outbound_rtp_and_sender_report_cadence();
  test_outbound_receiver_report_updates_report_block_fields();
  test_copy_to_progress_key();
  return 0;
}
