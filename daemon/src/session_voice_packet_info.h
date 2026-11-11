#pragma once

#include <cstddef>
#include <cstdint>

namespace agentd {

// Packet metadata is independent of the optional DTLS/SRTP transport.
struct ParsedRtpPacketInfo {
  uint8_t payload_type = 0;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
  size_t payload_offset = 0;
  size_t payload_size = 0;
};

struct ParsedRtcpPacketInfo {
  uint8_t packet_type = 0;
  uint8_t report_count = 0;
  uint16_t length_words = 0;
  uint32_t ssrc = 0;
  uint32_t sender_ntp_msw = 0;
  uint32_t sender_ntp_lsw = 0;
  uint32_t sender_rtp_timestamp = 0;
  uint32_t sender_packet_count = 0;
  uint32_t sender_octet_count = 0;
  uint32_t sender_report_lsr = 0;
  bool has_sender_info = false;
  size_t packet_size = 0;
  size_t compound_packet_size = 0;
  size_t compound_packet_count = 0;
  bool is_compound = false;
};

}  // namespace agentd
