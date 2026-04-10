#include "session_voice_dtls_srtp_util.h"

#include <openssl/err.h>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace agentd {
namespace {

constexpr unsigned long kDefaultReplayWindowSize = 1024;

struct ProfileNameMapping {
  const char* openssl_name;
  srtp_profile_t libsrtp_profile;
};

constexpr std::array<ProfileNameMapping, 4> kProfileMappings = {{
  {"SRTP_AES128_CM_SHA1_80", srtp_profile_aes128_cm_sha1_80},
  {"SRTP_AES128_CM_SHA1_32", srtp_profile_aes128_cm_sha1_32},
  {"SRTP_AEAD_AES_128_GCM", srtp_profile_aead_aes_128_gcm},
  {"SRTP_AEAD_AES_256_GCM", srtp_profile_aead_aes_256_gcm},
}};

bool set_crypto_policy_from_profile(
  srtp_profile_t profile,
  srtp_crypto_policy_t* rtp,
  srtp_crypto_policy_t* rtcp,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (!rtp || !rtcp) {
    if (out_err) *out_err = "missing libsrtp crypto policy output";
    return false;
  }
  const srtp_err_status_t rtp_status =
    srtp_crypto_policy_set_from_profile_for_rtp(rtp, profile);
  if (rtp_status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = "libsrtp RTP crypto policy init failed: " +
                 srtp_err_status_text(rtp_status);
    }
    return false;
  }
  const srtp_err_status_t rtcp_status =
    srtp_crypto_policy_set_from_profile_for_rtcp(rtcp, profile);
  if (rtcp_status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = "libsrtp RTCP crypto policy init failed: " +
                 srtp_err_status_text(rtcp_status);
    }
    return false;
  }
  return true;
}

std::vector<unsigned char> join_master_key_and_salt(
  const std::vector<unsigned char>& master_key,
  const std::vector<unsigned char>& master_salt) {
  std::vector<unsigned char> out;
  out.reserve(master_key.size() + master_salt.size());
  out.insert(out.end(), master_key.begin(), master_key.end());
  out.insert(out.end(), master_salt.begin(), master_salt.end());
  return out;
}

bool create_single_srtp_session(
  const DtlsSrtpProfileSpec& profile,
  srtp_ssrc_type_t ssrc_type,
  const std::vector<unsigned char>& master_key_and_salt,
  srtp_t* out_session,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_session) {
    if (out_err) *out_err = "missing libsrtp session output";
    return false;
  }
  *out_session = nullptr;
  if (master_key_and_salt.size() !=
      static_cast<size_t>(profile.master_key_len + profile.master_salt_len)) {
    if (out_err) *out_err = "invalid libsrtp master key/salt size";
    return false;
  }

  srtp_policy_t policy;
  std::memset(&policy, 0, sizeof(policy));
  if (!set_crypto_policy_from_profile(
        profile.libsrtp_profile, &policy.rtp, &policy.rtcp, out_err)) {
    return false;
  }
  policy.ssrc.type = ssrc_type;
  policy.ssrc.value = 0;
  policy.key = const_cast<unsigned char*>(master_key_and_salt.data());
  policy.window_size = kDefaultReplayWindowSize;
  policy.allow_repeat_tx = 1;
  policy.next = nullptr;

  const srtp_err_status_t status = srtp_create(out_session, &policy);
  if (status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = "libsrtp session creation failed: " + srtp_err_status_text(status);
    }
    return false;
  }
  return true;
}

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

}  // namespace

std::string openssl_dtls_last_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

std::string srtp_err_status_text(srtp_err_status_t status) {
  switch (status) {
    case srtp_err_status_ok:
      return "ok";
    case srtp_err_status_fail:
      return "fail";
    case srtp_err_status_bad_param:
      return "bad_param";
    case srtp_err_status_alloc_fail:
      return "alloc_fail";
    case srtp_err_status_dealloc_fail:
      return "dealloc_fail";
    case srtp_err_status_init_fail:
      return "init_fail";
    case srtp_err_status_terminus:
      return "terminus";
    case srtp_err_status_auth_fail:
      return "auth_fail";
    case srtp_err_status_cipher_fail:
      return "cipher_fail";
    case srtp_err_status_replay_fail:
      return "replay_fail";
    case srtp_err_status_replay_old:
      return "replay_old";
    case srtp_err_status_algo_fail:
      return "algo_fail";
    case srtp_err_status_no_such_op:
      return "no_such_op";
    case srtp_err_status_no_ctx:
      return "no_ctx";
    case srtp_err_status_cant_check:
      return "cant_check";
    case srtp_err_status_key_expired:
      return "key_expired";
    case srtp_err_status_socket_err:
      return "socket_err";
    case srtp_err_status_signal_err:
      return "signal_err";
    case srtp_err_status_nonce_bad:
      return "nonce_bad";
    case srtp_err_status_read_fail:
      return "read_fail";
    case srtp_err_status_write_fail:
      return "write_fail";
    case srtp_err_status_parse_err:
      return "parse_err";
    case srtp_err_status_encode_err:
      return "encode_err";
    case srtp_err_status_semaphore_err:
      return "semaphore_err";
    case srtp_err_status_pfkey_err:
      return "pfkey_err";
    case srtp_err_status_bad_mki:
      return "bad_mki";
    case srtp_err_status_pkt_idx_old:
      return "pkt_idx_old";
    case srtp_err_status_pkt_idx_adv:
      return "pkt_idx_adv";
    case srtp_err_status_cryptex_err:
      return "cryptex_err";
    default:
      return "status_" + std::to_string(static_cast<int>(status));
  }
}

std::string selected_dtls_srtp_profile_name(SSL* ssl) {
  if (!ssl) return "";
  SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(ssl);
  if (!profile || !profile->name) return "";
  return std::string(profile->name);
}

bool resolve_dtls_srtp_profile_spec(
  const std::string& profile_name,
  DtlsSrtpProfileSpec* out_spec,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_spec) {
    if (out_err) *out_err = "missing DTLS-SRTP profile output";
    return false;
  }
  const std::string trimmed = profile_name;
  for (const auto& mapping : kProfileMappings) {
    if (trimmed == mapping.openssl_name) {
      const unsigned int master_key_len =
        srtp_profile_get_master_key_length(mapping.libsrtp_profile);
      const unsigned int master_salt_len =
        srtp_profile_get_master_salt_length(mapping.libsrtp_profile);
      if (master_key_len == 0 || master_salt_len == 0) {
        if (out_err) {
          *out_err = "libsrtp reported invalid key sizes for profile " + trimmed;
        }
        return false;
      }
      out_spec->profile_name = trimmed;
      out_spec->libsrtp_profile = mapping.libsrtp_profile;
      out_spec->master_key_len = master_key_len;
      out_spec->master_salt_len = master_salt_len;
      out_spec->exporter_bytes = 2u * (master_key_len + master_salt_len);
      return true;
    }
  }
  if (out_err) *out_err = "unsupported DTLS-SRTP profile: " + trimmed;
  return false;
}

bool export_dtls_srtp_keying_material(
  SSL* ssl,
  const DtlsSrtpProfileSpec& profile,
  std::vector<unsigned char>* out_keying_material,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (out_keying_material) out_keying_material->clear();
  if (!ssl) {
    if (out_err) *out_err = "missing SSL handle for DTLS-SRTP exporter";
    return false;
  }
  if (profile.exporter_bytes == 0) {
    if (out_err) *out_err = "invalid DTLS-SRTP exporter size";
    return false;
  }
  std::vector<unsigned char> keying_material(profile.exporter_bytes, 0);
  const int rc = SSL_export_keying_material(
    ssl,
    keying_material.data(),
    keying_material.size(),
    kDtlsSrtpExporterLabel,
    std::strlen(kDtlsSrtpExporterLabel),
    nullptr,
    0,
    0);
  if (rc != 1) {
    if (out_err) *out_err = "openssl DTLS-SRTP exporter failed: " + openssl_dtls_last_error_text();
    return false;
  }
  if (out_keying_material) *out_keying_material = std::move(keying_material);
  return true;
}

bool derive_dtls_srtp_key_block(
  const DtlsSrtpProfileSpec& profile,
  const unsigned char* exporter_keying_material,
  size_t exporter_keying_material_size,
  DtlsSrtpKeyBlock* out_key_block,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_key_block) {
    if (out_err) *out_err = "missing DTLS-SRTP key block output";
    return false;
  }
  *out_key_block = DtlsSrtpKeyBlock();
  if (!exporter_keying_material) {
    if (out_err) *out_err = "missing DTLS-SRTP exporter keying material";
    return false;
  }
  if (exporter_keying_material_size != profile.exporter_bytes) {
    if (out_err) *out_err = "unexpected DTLS-SRTP exporter keying material size";
    return false;
  }

  const size_t key_len = profile.master_key_len;
  const size_t salt_len = profile.master_salt_len;
  const unsigned char* cursor = exporter_keying_material;
  out_key_block->profile = profile;
  out_key_block->client_master_key.assign(cursor, cursor + key_len);
  cursor += key_len;
  out_key_block->server_master_key.assign(cursor, cursor + key_len);
  cursor += key_len;
  out_key_block->client_master_salt.assign(cursor, cursor + salt_len);
  cursor += salt_len;
  out_key_block->server_master_salt.assign(cursor, cursor + salt_len);
  return true;
}

bool create_dtls_srtp_session_pair(
  const DtlsSrtpKeyBlock& key_block,
  DtlsSrtpLocalRole local_role,
  DtlsSrtpSessionPair* out_pair,
  std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_pair) {
    if (out_err) *out_err = "missing DTLS-SRTP session pair output";
    return false;
  }
  destroy_dtls_srtp_session_pair(out_pair);
  out_pair->profile_name = key_block.profile.profile_name;

  const bool local_is_server = local_role == DtlsSrtpLocalRole::server;
  const std::vector<unsigned char> outbound_master_key_and_salt =
    join_master_key_and_salt(
      local_is_server ? key_block.server_master_key : key_block.client_master_key,
      local_is_server ? key_block.server_master_salt : key_block.client_master_salt);
  const std::vector<unsigned char> inbound_master_key_and_salt =
    join_master_key_and_salt(
      local_is_server ? key_block.client_master_key : key_block.server_master_key,
      local_is_server ? key_block.client_master_salt : key_block.server_master_salt);

  std::string inbound_err;
  if (!create_single_srtp_session(
        key_block.profile,
        ssrc_any_inbound,
        inbound_master_key_and_salt,
        &out_pair->inbound,
        &inbound_err)) {
    if (out_err) *out_err = inbound_err;
    destroy_dtls_srtp_session_pair(out_pair);
    return false;
  }
  out_pair->inbound_ready = out_pair->inbound != nullptr;

  std::string outbound_err;
  if (!create_single_srtp_session(
        key_block.profile,
        ssrc_any_outbound,
        outbound_master_key_and_salt,
        &out_pair->outbound,
        &outbound_err)) {
    if (out_err) *out_err = outbound_err;
    destroy_dtls_srtp_session_pair(out_pair);
    return false;
  }
  out_pair->outbound_ready = out_pair->outbound != nullptr;
  return true;
}

bool is_probable_dtls_packet(const unsigned char* data, size_t size) {
  if (!data || size == 0) return false;
  const uint8_t first = data[0];
  return first >= 20 && first <= 63;
}

bool is_probable_rtp_or_rtcp_packet(const unsigned char* data, size_t size) {
  if (!data || size < 2) return false;
  return (data[0] & 0xC0u) == 0x80u;
}

bool is_probable_rtcp_packet(const unsigned char* data, size_t size) {
  if (!is_probable_rtp_or_rtcp_packet(data, size)) return false;
  const uint8_t packet_type = data[1];
  return packet_type >= 192 && packet_type <= 223;
}

bool parse_rtp_packet(
  const unsigned char* packet,
  size_t packet_size,
  ParsedRtpPacketInfo* out_info,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_info) *out_info = ParsedRtpPacketInfo();
  if (!packet || packet_size < 12) {
    if (out_err) *out_err = "RTP packet too short";
    return false;
  }
  if ((packet[0] & 0xC0u) != 0x80u) {
    if (out_err) *out_err = "unsupported RTP version";
    return false;
  }

  const bool has_padding = (packet[0] & 0x20u) != 0;
  const bool has_extension = (packet[0] & 0x10u) != 0;
  const size_t csrc_count = static_cast<size_t>(packet[0] & 0x0Fu);
  size_t header_size = 12 + (csrc_count * 4);
  if (packet_size < header_size) {
    if (out_err) *out_err = "RTP CSRC list truncated";
    return false;
  }
  if (has_extension) {
    if (packet_size < header_size + 4) {
      if (out_err) *out_err = "RTP extension header truncated";
      return false;
    }
    const uint16_t extension_words = read_u16_be(packet + header_size + 2);
    header_size += 4 + (static_cast<size_t>(extension_words) * 4);
    if (packet_size < header_size) {
      if (out_err) *out_err = "RTP extension payload truncated";
      return false;
    }
  }

  size_t payload_size = packet_size - header_size;
  if (has_padding) {
    const size_t padding_bytes = static_cast<size_t>(packet[packet_size - 1]);
    if (padding_bytes == 0 || padding_bytes > payload_size) {
      if (out_err) *out_err = "invalid RTP padding";
      return false;
    }
    payload_size -= padding_bytes;
  }

  if (out_info) {
    out_info->payload_type = static_cast<uint8_t>(packet[1] & 0x7Fu);
    out_info->sequence = read_u16_be(packet + 2);
    out_info->timestamp = read_u32_be(packet + 4);
    out_info->ssrc = read_u32_be(packet + 8);
    out_info->payload_offset = header_size;
    out_info->payload_size = payload_size;
  }
  return true;
}

bool parse_rtcp_packet(
  const unsigned char* packet,
  size_t packet_size,
  ParsedRtcpPacketInfo* out_info,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_info) *out_info = ParsedRtcpPacketInfo();
  if (!packet || packet_size < 8) {
    if (out_err) *out_err = "RTCP packet too short";
    return false;
  }
  if ((packet[0] & 0xC0u) != 0x80u) {
    if (out_err) *out_err = "unsupported RTCP version";
    return false;
  }
  if (!is_probable_rtcp_packet(packet, packet_size)) {
    if (out_err) *out_err = "packet is not RTCP";
    return false;
  }

  const uint16_t length_words = read_u16_be(packet + 2);
  const size_t declared_packet_size =
    (static_cast<size_t>(length_words) + static_cast<size_t>(1)) * static_cast<size_t>(4);
  if (declared_packet_size > packet_size) {
    if (out_err) *out_err = "RTCP packet length exceeds buffer";
    return false;
  }
  size_t compound_packet_size = declared_packet_size;
  size_t compound_packet_count = 1;
  while (compound_packet_size < packet_size) {
    if (packet_size - compound_packet_size < 4) {
      if (out_err) *out_err = "compound RTCP packet has trailing partial header";
      return false;
    }
    const unsigned char* subpacket = packet + compound_packet_size;
    if (!is_probable_rtcp_packet(subpacket, packet_size - compound_packet_size)) {
      if (out_err) *out_err = "compound RTCP packet contains non-RTCP member";
      return false;
    }
    const uint16_t subpacket_length_words = read_u16_be(subpacket + 2);
    const size_t subpacket_size =
      (static_cast<size_t>(subpacket_length_words) + static_cast<size_t>(1)) *
      static_cast<size_t>(4);
    if (subpacket_size == 0 || subpacket_size > packet_size - compound_packet_size) {
      if (out_err) *out_err = "compound RTCP packet member length exceeds buffer";
      return false;
    }
    compound_packet_size += subpacket_size;
    compound_packet_count += 1;
  }

  if (out_info) {
    out_info->packet_type = packet[1];
    out_info->report_count = static_cast<uint8_t>(packet[0] & 0x1Fu);
    out_info->length_words = length_words;
    out_info->ssrc = read_u32_be(packet + 4);
    if (out_info->packet_type == 200 && declared_packet_size >= 28) {
      out_info->sender_ntp_msw = read_u32_be(packet + 8);
      out_info->sender_ntp_lsw = read_u32_be(packet + 12);
      out_info->sender_rtp_timestamp = read_u32_be(packet + 16);
      out_info->sender_packet_count = read_u32_be(packet + 20);
      out_info->sender_octet_count = read_u32_be(packet + 24);
      out_info->sender_report_lsr =
        ((out_info->sender_ntp_msw & 0xFFFFu) << 16) |
        ((out_info->sender_ntp_lsw >> 16) & 0xFFFFu);
      out_info->has_sender_info = true;
    }
    out_info->packet_size = declared_packet_size;
    out_info->compound_packet_size = compound_packet_size;
    out_info->compound_packet_count = compound_packet_count;
    out_info->is_compound = compound_packet_count > 1;
  }
  return true;
}

bool unprotect_inbound_srtp_packet(
  srtp_t inbound_session,
  const unsigned char* packet,
  size_t packet_size,
  ParsedRtpPacketInfo* out_info,
  bool* out_was_rtcp,
  std::string* out_err,
  ParsedRtcpPacketInfo* out_rtcp_info
) {
  if (out_err) out_err->clear();
  if (out_info) *out_info = ParsedRtpPacketInfo();
  if (out_rtcp_info) *out_rtcp_info = ParsedRtcpPacketInfo();
  if (out_was_rtcp) *out_was_rtcp = false;
  if (!inbound_session) {
    if (out_err) *out_err = "missing inbound SRTP session";
    return false;
  }
  if (!packet || packet_size == 0) {
    if (out_err) *out_err = "missing inbound SRTP packet";
    return false;
  }

  std::vector<unsigned char> mutable_packet(packet, packet + packet_size);
  int mutable_len = static_cast<int>(mutable_packet.size());
  const bool rtcp = is_probable_rtcp_packet(mutable_packet.data(), mutable_packet.size());
  const srtp_err_status_t status = rtcp
    ? srtp_unprotect_rtcp(inbound_session, mutable_packet.data(), &mutable_len)
    : srtp_unprotect(inbound_session, mutable_packet.data(), &mutable_len);
  if (status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = std::string(rtcp ? "inbound SRTCP unprotect failed: " : "inbound SRTP unprotect failed: ") +
                 srtp_err_status_text(status);
    }
    return false;
  }

  if (out_was_rtcp) *out_was_rtcp = rtcp;
  if (rtcp) {
    return parse_rtcp_packet(
      mutable_packet.data(),
      static_cast<size_t>(mutable_len),
      out_rtcp_info,
      out_err);
  }
  return parse_rtp_packet(
    mutable_packet.data(),
    static_cast<size_t>(mutable_len),
    out_info,
    out_err);
}

bool protect_outbound_rtp_packet(
  srtp_t outbound_session,
  const unsigned char* packet,
  size_t packet_size,
  std::vector<unsigned char>* out_protected_packet,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_protected_packet) out_protected_packet->clear();
  if (!outbound_session) {
    if (out_err) *out_err = "missing outbound SRTP session";
    return false;
  }
  if (!packet || packet_size == 0) {
    if (out_err) *out_err = "missing outbound RTP packet";
    return false;
  }
  if (!is_probable_rtp_or_rtcp_packet(packet, packet_size) ||
      is_probable_rtcp_packet(packet, packet_size)) {
    if (out_err) *out_err = "outbound packet is not RTP";
    return false;
  }
  if (!out_protected_packet) {
    if (out_err) *out_err = "missing protected RTP output";
    return false;
  }
  if (packet_size > static_cast<size_t>(std::numeric_limits<int>::max() - SRTP_MAX_TRAILER_LEN)) {
    if (out_err) *out_err = "outbound RTP packet too large";
    return false;
  }

  std::vector<unsigned char> mutable_packet(packet, packet + packet_size);
  mutable_packet.resize(packet_size + SRTP_MAX_TRAILER_LEN);
  int mutable_len = static_cast<int>(packet_size);
  const srtp_err_status_t status =
    srtp_protect(outbound_session, mutable_packet.data(), &mutable_len);
  if (status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = "outbound SRTP protect failed: " + srtp_err_status_text(status);
    }
    return false;
  }
  if (mutable_len <= 0) {
    if (out_err) *out_err = "outbound SRTP protect returned an empty packet";
    return false;
  }
  mutable_packet.resize(static_cast<size_t>(mutable_len));
  *out_protected_packet = std::move(mutable_packet);
  return true;
}

bool protect_outbound_rtcp_packet(
  srtp_t outbound_session,
  const unsigned char* packet,
  size_t packet_size,
  std::vector<unsigned char>* out_protected_packet,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_protected_packet) out_protected_packet->clear();
  if (!outbound_session) {
    if (out_err) *out_err = "missing outbound SRTCP session";
    return false;
  }
  if (!packet || packet_size == 0) {
    if (out_err) *out_err = "missing outbound RTCP packet";
    return false;
  }
  if (!is_probable_rtcp_packet(packet, packet_size)) {
    if (out_err) *out_err = "outbound packet is not RTCP";
    return false;
  }
  if (!out_protected_packet) {
    if (out_err) *out_err = "missing protected RTCP output";
    return false;
  }
  ParsedRtcpPacketInfo rtcp_info;
  if (!parse_rtcp_packet(packet, packet_size, &rtcp_info, out_err)) return false;
  if (rtcp_info.compound_packet_size != packet_size) {
    if (out_err) *out_err = "outbound RTCP packet length mismatch";
    return false;
  }
  if (packet_size > static_cast<size_t>(std::numeric_limits<int>::max() - SRTP_MAX_TRAILER_LEN)) {
    if (out_err) *out_err = "outbound RTCP packet too large";
    return false;
  }

  std::vector<unsigned char> mutable_packet(packet, packet + packet_size);
  mutable_packet.resize(packet_size + SRTP_MAX_TRAILER_LEN);
  int mutable_len = static_cast<int>(packet_size);
  const srtp_err_status_t status =
    srtp_protect_rtcp(outbound_session, mutable_packet.data(), &mutable_len);
  if (status != srtp_err_status_ok) {
    if (out_err) {
      *out_err = "outbound SRTCP protect failed: " + srtp_err_status_text(status);
    }
    return false;
  }
  if (mutable_len <= 0) {
    if (out_err) *out_err = "outbound SRTCP protect returned an empty packet";
    return false;
  }
  mutable_packet.resize(static_cast<size_t>(mutable_len));
  *out_protected_packet = std::move(mutable_packet);
  return true;
}

void destroy_dtls_srtp_session_pair(DtlsSrtpSessionPair* pair) {
  if (!pair) return;
  if (pair->inbound) {
    (void)srtp_dealloc(pair->inbound);
    pair->inbound = nullptr;
  }
  if (pair->outbound) {
    (void)srtp_dealloc(pair->outbound);
    pair->outbound = nullptr;
  }
  pair->inbound_ready = false;
  pair->outbound_ready = false;
  pair->profile_name.clear();
}

}  // namespace agentd
