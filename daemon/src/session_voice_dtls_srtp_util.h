#pragma once

#include <openssl/ssl.h>
#include <srtp2/srtp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

inline constexpr const char* kDtlsSrtpExporterLabel = "EXTRACTOR-dtls_srtp";

enum class DtlsSrtpLocalRole {
  client,
  server,
};

struct DtlsSrtpProfileSpec {
  std::string profile_name;
  srtp_profile_t libsrtp_profile = srtp_profile_reserved;
  unsigned int master_key_len = 0;
  unsigned int master_salt_len = 0;
  size_t exporter_bytes = 0;
};

struct DtlsSrtpKeyBlock {
  DtlsSrtpProfileSpec profile;
  std::vector<unsigned char> client_master_key;
  std::vector<unsigned char> client_master_salt;
  std::vector<unsigned char> server_master_key;
  std::vector<unsigned char> server_master_salt;
};

struct DtlsSrtpSessionPair {
  srtp_t inbound = nullptr;
  srtp_t outbound = nullptr;
  std::string profile_name;
  bool inbound_ready = false;
  bool outbound_ready = false;
};

struct ParsedRtpPacketInfo {
  uint8_t payload_type = 0;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
  size_t payload_offset = 0;
  size_t payload_size = 0;
};

std::string openssl_dtls_last_error_text();
std::string srtp_err_status_text(srtp_err_status_t status);
std::string selected_dtls_srtp_profile_name(SSL* ssl);

bool resolve_dtls_srtp_profile_spec(
  const std::string& profile_name,
  DtlsSrtpProfileSpec* out_spec,
  std::string* out_err);

bool export_dtls_srtp_keying_material(
  SSL* ssl,
  const DtlsSrtpProfileSpec& profile,
  std::vector<unsigned char>* out_keying_material,
  std::string* out_err);

bool derive_dtls_srtp_key_block(
  const DtlsSrtpProfileSpec& profile,
  const unsigned char* exporter_keying_material,
  size_t exporter_keying_material_size,
  DtlsSrtpKeyBlock* out_key_block,
  std::string* out_err);

bool create_dtls_srtp_session_pair(
  const DtlsSrtpKeyBlock& key_block,
  DtlsSrtpLocalRole local_role,
  DtlsSrtpSessionPair* out_pair,
  std::string* out_err);

bool is_probable_dtls_packet(const unsigned char* data, size_t size);
bool is_probable_rtp_or_rtcp_packet(const unsigned char* data, size_t size);
bool is_probable_rtcp_packet(const unsigned char* data, size_t size);

bool parse_rtp_packet(
  const unsigned char* packet,
  size_t packet_size,
  ParsedRtpPacketInfo* out_info,
  std::string* out_err);

bool unprotect_inbound_srtp_packet(
  srtp_t inbound_session,
  const unsigned char* packet,
  size_t packet_size,
  ParsedRtpPacketInfo* out_info,
  bool* out_was_rtcp,
  std::string* out_err);

void destroy_dtls_srtp_session_pair(DtlsSrtpSessionPair* pair);

}  // namespace agentd
