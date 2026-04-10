#include "session_voice_audio_decode.h"
#include "session_voice_builtin_media_engine_plugin.h"
#include "session_voice_dtls_srtp_util.h"

#include <juice/juice.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>
#include <openssl/x509.h>
#include <srtp2/srtp.h>
#include <usrsctp.h>

#if defined(AGENTD_HAVE_OPUS)
#include <opus.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using agentd::DtlsSrtpKeyBlock;
using agentd::DtlsSrtpLocalRole;
using agentd::DtlsSrtpProfileSpec;
using agentd::DtlsSrtpSessionPair;
using agentd::create_dtls_srtp_session_pair;
using agentd::derive_dtls_srtp_key_block;
using agentd::export_dtls_srtp_keying_material;
using agentd::resolve_dtls_srtp_profile_spec;
using agentd::selected_dtls_srtp_profile_name;

constexpr const char* kProviderName = "agentd_builtin_embedded_transport_provider";
constexpr const char* kProviderVersion = "0.11.0";
#if defined(AGENTD_HAVE_OPUS)
constexpr const char* kCapabilitiesJson =
  "{\"signaling\":true,\"audio_capture\":false,\"audio_render\":false,"
  "\"audio_decode\":true,\"audio_stage\":true,\"audio_drain\":true,"
  "\"audio_owner_handoff\":true,\"audio_submit\":true,"
  "\"audio_outbound_pcmu\":true,\"audio_outbound_pcma\":true,\"audio_outbound_opus\":true,"
  "\"audio_codec_pcmu\":true,\"audio_codec_pcma\":true,\"audio_codec_opus\":true,"
  "\"ice\":true,\"dtls\":true,\"dtls_identity\":true,\"dtls_answer_shape\":true,"
  "\"dtls_handshake\":true,\"dtls_srtp_export\":true,"
  "\"srtp_contexts\":true,\"poll_status\":true,"
  "\"srtp\":true,\"rtp_ingest\":true,\"rtp_transmit\":true,"
  "\"rtcp_ingest\":true,\"rtcp_transmit\":true,\"sctp\":true,"
  "\"transport_family\":\"embedded_transport_primitives\","
  "\"embedded_transport_provider\":true,\"sample_provider\":false,"
  "\"real_media_engine\":false,\"remote_description_optional\":true,"
  "\"candidate_trickle_ingest\":true,\"candidate_trickle_emit\":false}";
#else
constexpr const char* kCapabilitiesJson =
  "{\"signaling\":true,\"audio_capture\":false,\"audio_render\":false,"
  "\"audio_decode\":true,\"audio_stage\":true,\"audio_drain\":true,"
  "\"audio_owner_handoff\":true,\"audio_submit\":true,"
  "\"audio_outbound_pcmu\":true,\"audio_outbound_pcma\":true,\"audio_outbound_opus\":false,"
  "\"audio_codec_pcmu\":true,\"audio_codec_pcma\":true,\"audio_codec_opus\":false,"
  "\"ice\":true,\"dtls\":true,\"dtls_identity\":true,\"dtls_answer_shape\":true,"
  "\"dtls_handshake\":true,\"dtls_srtp_export\":true,"
  "\"srtp_contexts\":true,\"poll_status\":true,"
  "\"srtp\":true,\"rtp_ingest\":true,\"rtp_transmit\":true,"
  "\"rtcp_ingest\":true,\"rtcp_transmit\":true,\"sctp\":true,"
  "\"transport_family\":\"embedded_transport_primitives\","
  "\"embedded_transport_provider\":true,\"sample_provider\":false,"
  "\"real_media_engine\":false,\"remote_description_optional\":true,"
  "\"candidate_trickle_ingest\":true,\"candidate_trickle_emit\":false}";
#endif

constexpr const char* kDtlsSrtpProfiles =
  "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32";
constexpr long kDtlsDatagramMtu = 1200;
constexpr size_t kAudioPcmStagingCapacitySamples = 48000 * 2 * 2;
constexpr size_t kOutboundG711FrameSamples = 160;
constexpr size_t kOutboundOpusFrameSamplesPerChannel = 960;
constexpr int kOutboundOpusMaxPayloadBytes = 1275;
constexpr uint32_t kOutboundRtpSsrc = 0xA6E17D01u;
constexpr uint8_t kRtcpPacketTypeSenderReport = 200;
constexpr size_t kRtcpSenderReportBytes = 28;
constexpr uint64_t kUnixToNtpEpochSeconds = 2208988800ULL;

struct EmbeddedTransportState {
  struct AsyncProgressKey {
    std::string libjuice_state;
    std::string dtls_handshake_state;
    std::string dtls_selected_srtp_profile;
    std::string srtp_last_error;
    std::string last_remote_description_error;
    std::string selected_local_candidate;
    std::string selected_remote_candidate;
    std::string selected_local_address;
    std::string selected_remote_address;
    uint64_t offers_seen = 0;
    uint64_t remote_candidates_seen = 0;
    uint64_t local_candidates_observed = 0;
    bool gathering_done = false;
    bool gather_started = false;
    bool remote_description_applied = false;
    bool transport_connectivity_ready = false;
    bool dtls_identity_ready = false;
    bool dtls_handshake_ready = false;
    bool dtls_exporter_ready = false;
    bool srtp_contexts_ready = false;
    bool srtp_inbound_ready = false;
    bool srtp_outbound_ready = false;
    uint64_t rtp_packets_received = 0;
    uint64_t rtp_payload_bytes_received = 0;
    uint64_t rtp_packets_sent = 0;
    uint64_t rtp_payload_bytes_sent = 0;
    uint64_t rtcp_packets_received = 0;
    uint64_t rtcp_packets_sent = 0;
    uint64_t rtcp_payload_bytes_received = 0;
    uint64_t rtcp_payload_bytes_sent = 0;
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
    uint64_t audio_frames_decoded = 0;
    uint64_t audio_pcm_samples_decoded = 0;
    uint64_t audio_pcm_samples_buffered = 0;
    uint64_t audio_outbound_frames_sent = 0;
    uint64_t audio_pcm_samples_submitted_total = 0;
    uint64_t audio_last_outbound_samples = 0;
    int64_t audio_outbound_payload_type = -1;
    int64_t audio_outbound_sample_rate_hz = 0;
    int64_t audio_outbound_channels = 0;
    std::string audio_outbound_codec_name;
    int64_t audio_last_sample_rate_hz = 0;
    int64_t audio_last_channels = 0;
    int64_t audio_last_frame_samples_per_channel = 0;
    std::string audio_last_codec_name;
    std::string audio_last_error;
    std::string audio_outbound_last_error;
    std::string rtcp_last_error;

    bool operator==(const AsyncProgressKey& other) const {
      return libjuice_state == other.libjuice_state &&
             dtls_handshake_state == other.dtls_handshake_state &&
             dtls_selected_srtp_profile == other.dtls_selected_srtp_profile &&
             srtp_last_error == other.srtp_last_error &&
             last_remote_description_error == other.last_remote_description_error &&
             selected_local_candidate == other.selected_local_candidate &&
             selected_remote_candidate == other.selected_remote_candidate &&
             selected_local_address == other.selected_local_address &&
             selected_remote_address == other.selected_remote_address &&
             offers_seen == other.offers_seen &&
             remote_candidates_seen == other.remote_candidates_seen &&
             local_candidates_observed == other.local_candidates_observed &&
             gathering_done == other.gathering_done &&
             gather_started == other.gather_started &&
             remote_description_applied == other.remote_description_applied &&
             transport_connectivity_ready == other.transport_connectivity_ready &&
             dtls_identity_ready == other.dtls_identity_ready &&
             dtls_handshake_ready == other.dtls_handshake_ready &&
             dtls_exporter_ready == other.dtls_exporter_ready &&
             srtp_contexts_ready == other.srtp_contexts_ready &&
             srtp_inbound_ready == other.srtp_inbound_ready &&
             srtp_outbound_ready == other.srtp_outbound_ready &&
             rtp_packets_received == other.rtp_packets_received &&
             rtp_payload_bytes_received == other.rtp_payload_bytes_received &&
             rtp_packets_sent == other.rtp_packets_sent &&
             rtp_payload_bytes_sent == other.rtp_payload_bytes_sent &&
             rtcp_packets_received == other.rtcp_packets_received &&
             rtcp_packets_sent == other.rtcp_packets_sent &&
             rtcp_payload_bytes_received == other.rtcp_payload_bytes_received &&
             rtcp_payload_bytes_sent == other.rtcp_payload_bytes_sent &&
             rtp_last_payload_type == other.rtp_last_payload_type &&
             rtp_last_sequence == other.rtp_last_sequence &&
             rtp_last_timestamp == other.rtp_last_timestamp &&
             rtp_last_ssrc == other.rtp_last_ssrc &&
             rtp_last_sent_payload_type == other.rtp_last_sent_payload_type &&
             rtp_last_sent_sequence == other.rtp_last_sent_sequence &&
             rtp_last_sent_timestamp == other.rtp_last_sent_timestamp &&
             rtp_last_sent_ssrc == other.rtp_last_sent_ssrc &&
             rtcp_last_packet_type == other.rtcp_last_packet_type &&
             rtcp_last_ssrc == other.rtcp_last_ssrc &&
             rtcp_last_sent_packet_type == other.rtcp_last_sent_packet_type &&
             rtcp_last_sent_ssrc == other.rtcp_last_sent_ssrc &&
             audio_frames_decoded == other.audio_frames_decoded &&
             audio_pcm_samples_decoded == other.audio_pcm_samples_decoded &&
             audio_pcm_samples_buffered == other.audio_pcm_samples_buffered &&
             audio_outbound_frames_sent == other.audio_outbound_frames_sent &&
             audio_pcm_samples_submitted_total == other.audio_pcm_samples_submitted_total &&
             audio_last_outbound_samples == other.audio_last_outbound_samples &&
             audio_outbound_payload_type == other.audio_outbound_payload_type &&
             audio_outbound_sample_rate_hz == other.audio_outbound_sample_rate_hz &&
             audio_outbound_channels == other.audio_outbound_channels &&
             audio_outbound_codec_name == other.audio_outbound_codec_name &&
             audio_last_sample_rate_hz == other.audio_last_sample_rate_hz &&
             audio_last_channels == other.audio_last_channels &&
             audio_last_frame_samples_per_channel ==
               other.audio_last_frame_samples_per_channel &&
             audio_last_codec_name == other.audio_last_codec_name &&
             audio_last_error == other.audio_last_error &&
             audio_outbound_last_error == other.audio_outbound_last_error &&
             rtcp_last_error == other.rtcp_last_error;
    }
  };

  juice_agent_t* agent = nullptr;
  std::string local_description;
  std::string libjuice_state = "disconnected";
  std::string last_remote_description_error;
  SSL_CTX* dtls_ctx = nullptr;
  SSL* dtls_ssl = nullptr;
  srtp_t inbound_srtp = nullptr;
  srtp_t outbound_srtp = nullptr;
  EVP_PKEY* dtls_private_key = nullptr;
  X509* dtls_certificate = nullptr;
  std::string dtls_fingerprint_sha256;
  std::string dtls_setup_role = "passive";
  std::string dtls_certificate_subject;
  std::string dtls_handshake_state = "idle";
  std::string dtls_selected_srtp_profile;
  std::string dtls_last_error;
  std::string srtp_last_error;
  std::string selected_local_candidate;
  std::string selected_remote_candidate;
  std::string selected_local_address;
  std::string selected_remote_address;
  std::string last_answer_sdp_shape = "ice_only";
  uint64_t dtls_packets_sent = 0;
  uint64_t dtls_packets_received = 0;
  uint64_t offers_seen = 0;
  uint64_t remote_candidates_seen = 0;
  uint64_t local_candidates_observed = 0;
  bool gathering_done = false;
  bool gather_started = false;
  bool remote_description_applied = false;
  bool transport_connectivity_ready = false;
  bool dtls_identity_ready = false;
  bool dtls_handshake_ready = false;
  bool dtls_exporter_ready = false;
  bool srtp_contexts_ready = false;
  bool srtp_inbound_ready = false;
  bool srtp_outbound_ready = false;
  uint64_t rtp_packets_received = 0;
  uint64_t rtp_payload_bytes_received = 0;
  uint64_t rtp_packets_sent = 0;
  uint64_t rtp_payload_bytes_sent = 0;
  uint64_t rtcp_packets_received = 0;
  uint64_t rtcp_packets_sent = 0;
  uint64_t rtcp_payload_bytes_received = 0;
  uint64_t rtcp_payload_bytes_sent = 0;
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
  uint64_t audio_frames_decoded = 0;
  uint64_t audio_pcm_samples_decoded = 0;
  uint64_t audio_pcm_samples_buffered = 0;
  uint64_t audio_outbound_frames_sent = 0;
  uint64_t audio_pcm_samples_submitted_total = 0;
  uint64_t audio_last_outbound_samples = 0;
  int64_t audio_outbound_payload_type = -1;
  int64_t audio_outbound_sample_rate_hz = 0;
  int64_t audio_outbound_channels = 0;
  std::string audio_outbound_codec_name;
  int64_t audio_last_sample_rate_hz = 0;
  int64_t audio_last_channels = 0;
  int64_t audio_last_frame_samples_per_channel = 0;
  std::string audio_last_codec_name;
  std::string audio_last_error;
  std::string audio_outbound_last_error;
  std::string rtcp_last_error;
  uint16_t outbound_rtp_sequence = 1;
  uint32_t outbound_rtp_timestamp = 0;
  uint32_t outbound_rtp_ssrc = kOutboundRtpSsrc;
#if defined(AGENTD_HAVE_OPUS)
  OpusEncoder* outbound_opus_encoder = nullptr;
  int outbound_opus_sample_rate_hz = 0;
  int outbound_opus_channels = 0;
#endif
  agentd::InboundRtpAudioDecoder audio_decoder;
  std::deque<int16_t> pcm_staging;
  std::mutex async_events_mu;
  std::deque<std::string> pending_async_events;
  AsyncProgressKey last_async_key;
  bool last_async_key_initialized = false;
};

std::string build_event_json(
  const EmbeddedTransportState& state,
  const std::string& event_name,
  const std::string& media_engine_state,
  uint64_t initial_remote_candidate_count
);

std::mutex g_transport_runtime_mu;
size_t g_transport_runtime_refs = 0;
bool g_srtp_initialized = false;
bool g_usrsctp_initialized = false;

bool copy_text(const std::string& value, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  if (value.size() + 1 > out_size) return false;
  std::memset(out, 0, out_size);
  std::memcpy(out, value.data(), value.size());
  out[value.size()] = '\0';
  return true;
}

void write_error(const std::string& value, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  (void)copy_text(value.empty() ? std::string("unknown") : value, out, out_size);
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string openssl_last_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

std::string trim_copy(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    begin += 1;
  }
  size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    end -= 1;
  }
  return value.substr(begin, end - begin);
}

std::string juice_state_text(juice_state_t state) {
  const char* raw = juice_state_to_string(state);
  return raw ? std::string(raw) : std::string("unknown");
}

std::string x509_name_to_string(X509_NAME* name) {
  if (!name) return "";
  char* raw = X509_NAME_oneline(name, nullptr, 0);
  if (!raw) return "";
  std::string out = raw;
  OPENSSL_free(raw);
  return out;
}

std::string sha256_fingerprint_text(X509* cert) {
  if (!cert) return "";
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1 || md_len == 0) return "";
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(md_len * 3);
  for (unsigned int i = 0; i < md_len; ++i) {
    if (i > 0) out.push_back(':');
    out.push_back(kHex[(md[i] >> 4) & 0x0F]);
    out.push_back(kHex[md[i] & 0x0F]);
  }
  return out;
}

std::vector<std::string> split_sdp_lines(const std::string& sdp) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < sdp.size()) {
    size_t end = sdp.find('\n', start);
    std::string line = end == std::string::npos ? sdp.substr(start) : sdp.substr(start, end - start);
    line = trim_copy(line);
    if (!line.empty()) out.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

std::string join_sdp_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += "\r\n";
  }
  return out;
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool is_end_of_candidates_marker(const std::string& value) {
  return value == "a=end-of-candidates" || value == "end-of-candidates";
}

bool contains_media_section(const std::vector<std::string>& lines) {
  return std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
    return starts_with(line, "m=");
  });
}

std::vector<std::string> local_ice_lines_from_description(const std::string& sdp) {
  std::vector<std::string> out;
  for (const auto& line : split_sdp_lines(sdp)) {
    if (starts_with(line, "a=ice-ufrag:") ||
        starts_with(line, "a=ice-pwd:") ||
        starts_with(line, "a=ice-options:") ||
        starts_with(line, "a=candidate:") ||
        line == "a=end-of-candidates") {
      out.push_back(line);
    }
  }
  return out;
}

std::string rewrite_mline_for_inactive_answer(const std::string& line) {
  if (!starts_with(line, "m=")) return line;
  std::vector<std::string> parts;
  size_t start = 2;
  while (start <= line.size()) {
    const size_t next = line.find(' ', start);
    parts.push_back(next == std::string::npos ? line.substr(start) : line.substr(start, next - start));
    if (next == std::string::npos) break;
    start = next + 1;
  }
  if (parts.size() >= 2) parts[1] = "9";
  std::string out = "m=";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.push_back(' ');
    out += parts[i];
  }
  return out;
}

bool should_copy_session_attribute(const std::string& line) {
  return starts_with(line, "a=group:") || starts_with(line, "a=msid-semantic:");
}

bool should_copy_media_attribute(const std::string& line) {
  return starts_with(line, "a=mid:") ||
         starts_with(line, "a=rtcp-mux") ||
         starts_with(line, "a=rtcp-rsize") ||
         starts_with(line, "a=rtcp-mux-only") ||
         starts_with(line, "a=rtpmap:") ||
         starts_with(line, "a=fmtp:") ||
         starts_with(line, "a=rtcp-fb:") ||
         starts_with(line, "a=extmap:") ||
         starts_with(line, "a=extmap-allow-mixed") ||
         starts_with(line, "a=sctp-port:") ||
         starts_with(line, "a=max-message-size:");
}

bool generate_dtls_identity(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->dtls_identity_ready) return true;

  using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
  using ASN1IntPtr = std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)>;
  using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

  PkeyCtxPtr keygen_ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  if (!keygen_ctx) {
    if (out_err) *out_err = "openssl keygen ctx allocation failed: " + openssl_last_error_text();
    return false;
  }
  if (EVP_PKEY_keygen_init(keygen_ctx.get()) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(keygen_ctx.get(), 2048) != 1) {
    if (out_err) *out_err = "openssl keygen init failed: " + openssl_last_error_text();
    return false;
  }

  EVP_PKEY* raw_key = nullptr;
  if (EVP_PKEY_keygen(keygen_ctx.get(), &raw_key) != 1 || !raw_key) {
    if (out_err) *out_err = "openssl key generation failed: " + openssl_last_error_text();
    return false;
  }
  PkeyPtr key(raw_key, EVP_PKEY_free);

  X509Ptr cert(X509_new(), X509_free);
  if (!cert) {
    if (out_err) *out_err = "openssl x509 allocation failed: " + openssl_last_error_text();
    return false;
  }
  if (X509_set_version(cert.get(), 2) != 1) {
    if (out_err) *out_err = "openssl x509 version init failed: " + openssl_last_error_text();
    return false;
  }

  unsigned char serial_bytes[8];
  if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) {
    if (out_err) *out_err = "openssl random serial generation failed: " + openssl_last_error_text();
    return false;
  }
  BnPtr serial_bn(BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr), BN_free);
  if (!serial_bn) {
    if (out_err) *out_err = "openssl serial bignum allocation failed: " + openssl_last_error_text();
    return false;
  }
  ASN1IntPtr serial(BN_to_ASN1_INTEGER(serial_bn.get(), nullptr), ASN1_INTEGER_free);
  if (!serial || X509_set_serialNumber(cert.get(), serial.get()) != 1) {
    if (out_err) *out_err = "openssl serial assignment failed: " + openssl_last_error_text();
    return false;
  }

  if (!X509_gmtime_adj(X509_get_notBefore(cert.get()), 0) ||
      !X509_gmtime_adj(X509_get_notAfter(cert.get()), 7 * 24 * 60 * 60L) ||
      X509_set_pubkey(cert.get(), key.get()) != 1) {
    if (out_err) *out_err = "openssl certificate setup failed: " + openssl_last_error_text();
    return false;
  }

  X509_NAME* name = X509_get_subject_name(cert.get());
  if (!name ||
      X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("agentd builtin embedded transport"),
        -1,
        -1,
        0) != 1 ||
      X509_set_issuer_name(cert.get(), name) != 1) {
    if (out_err) *out_err = "openssl certificate subject init failed: " + openssl_last_error_text();
    return false;
  }

  if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
    if (out_err) *out_err = "openssl certificate signing failed: " + openssl_last_error_text();
    return false;
  }

  engine->dtls_fingerprint_sha256 = sha256_fingerprint_text(cert.get());
  engine->dtls_certificate_subject = x509_name_to_string(X509_get_subject_name(cert.get()));
  if (engine->dtls_fingerprint_sha256.empty()) {
    if (out_err) *out_err = "openssl certificate fingerprint generation failed";
    return false;
  }
  engine->dtls_private_key = key.release();
  engine->dtls_certificate = cert.release();
  engine->dtls_identity_ready = true;
  return true;
}

std::string ssl_error_text(SSL* ssl, int rc) {
  const int ssl_err = ssl ? SSL_get_error(ssl, rc) : SSL_ERROR_SSL;
  switch (ssl_err) {
    case SSL_ERROR_NONE:
      return "ssl_error_none";
    case SSL_ERROR_WANT_READ:
      return "ssl_want_read";
    case SSL_ERROR_WANT_WRITE:
      return "ssl_want_write";
    case SSL_ERROR_ZERO_RETURN:
      return "ssl_zero_return";
    case SSL_ERROR_SYSCALL:
      return "ssl_syscall";
    case SSL_ERROR_SSL:
    default:
      break;
  }
  return "ssl_error_" + std::to_string(ssl_err) + ": " + openssl_last_error_text();
}

void mark_dtls_failure(EmbeddedTransportState* engine, const std::string& err) {
  if (!engine) return;
  engine->dtls_handshake_state = "failed";
  engine->dtls_last_error = err;
}

bool ensure_srtp_contexts(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->srtp_contexts_ready) return true;
  if (!engine->dtls_ssl || !engine->dtls_handshake_ready) {
    if (out_err) *out_err = "DTLS handshake not ready for SRTP context creation";
    return false;
  }
  if (engine->dtls_selected_srtp_profile.empty()) {
    if (out_err) *out_err = "missing negotiated DTLS-SRTP profile";
    return false;
  }

  DtlsSrtpProfileSpec profile;
  std::string err;
  if (!resolve_dtls_srtp_profile_spec(engine->dtls_selected_srtp_profile, &profile, &err)) {
    engine->srtp_last_error = err;
    if (out_err) *out_err = err;
    return false;
  }

  std::vector<unsigned char> exporter_keying_material;
  if (!export_dtls_srtp_keying_material(
        engine->dtls_ssl, profile, &exporter_keying_material, &err)) {
    engine->srtp_last_error = err;
    if (out_err) *out_err = err;
    return false;
  }
  engine->dtls_exporter_ready = true;

  DtlsSrtpKeyBlock key_block;
  if (!derive_dtls_srtp_key_block(
        profile,
        exporter_keying_material.data(),
        exporter_keying_material.size(),
        &key_block,
        &err)) {
    engine->srtp_last_error = err;
    if (out_err) *out_err = err;
    return false;
  }

  DtlsSrtpSessionPair sessions;
  if (!create_dtls_srtp_session_pair(key_block, DtlsSrtpLocalRole::server, &sessions, &err)) {
    engine->srtp_last_error = err;
    if (out_err) *out_err = err;
    return false;
  }

  engine->inbound_srtp = sessions.inbound;
  engine->outbound_srtp = sessions.outbound;
  sessions.inbound = nullptr;
  sessions.outbound = nullptr;
  engine->srtp_inbound_ready = engine->inbound_srtp != nullptr;
  engine->srtp_outbound_ready = engine->outbound_srtp != nullptr;
  engine->srtp_contexts_ready = engine->srtp_inbound_ready && engine->srtp_outbound_ready;
  engine->srtp_last_error.clear();
  return engine->srtp_contexts_ready;
}

void stage_decoded_audio_frame(
  EmbeddedTransportState* engine,
  const agentd::DecodedAudioFrame& frame
) {
  if (!engine) return;
  engine->audio_frames_decoded += 1;
  engine->audio_pcm_samples_decoded += frame.pcm_samples.size();
  engine->audio_last_codec_name = frame.codec_name;
  engine->audio_last_sample_rate_hz = frame.sample_rate_hz;
  engine->audio_last_channels = frame.channels;
  engine->audio_last_frame_samples_per_channel = frame.samples_per_channel;
  engine->audio_last_error.clear();
  for (const int16_t sample : frame.pcm_samples) {
    engine->pcm_staging.push_back(sample);
  }
  while (engine->pcm_staging.size() > kAudioPcmStagingCapacitySamples) {
    engine->pcm_staging.pop_front();
  }
  engine->audio_pcm_samples_buffered = engine->pcm_staging.size();
}

bool decode_inbound_audio_frame(
  EmbeddedTransportState* engine,
  uint8_t payload_type,
  const unsigned char* payload,
  size_t payload_size,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!engine || !payload || payload_size == 0) {
    if (out_err) *out_err = "missing inbound audio payload";
    return false;
  }
  agentd::DecodedAudioFrame frame;
  std::string decode_err;
  if (!engine->audio_decoder.decode_payload(
        payload_type, payload, payload_size, &frame, &decode_err)) {
    engine->audio_last_error = decode_err;
    if (out_err) *out_err = decode_err;
    return false;
  }
  stage_decoded_audio_frame(engine, frame);
  return true;
}

void clear_outbound_audio_payload_selection(EmbeddedTransportState* engine) {
  if (!engine) return;
  engine->audio_outbound_payload_type = -1;
  engine->audio_outbound_sample_rate_hz = 0;
  engine->audio_outbound_channels = 0;
  engine->audio_outbound_codec_name.clear();
}

bool select_outbound_audio_payload_from_remote_sdp(EmbeddedTransportState* engine) {
  if (!engine) return false;
  clear_outbound_audio_payload_selection(engine);

  const agentd::RtpAudioPayloadSpec* selected = nullptr;
  for (const auto& spec : engine->audio_decoder.payload_specs()) {
#if defined(AGENTD_HAVE_OPUS)
    if (spec.codec_name == "OPUS" &&
        spec.sample_rate_hz == 48000 &&
        (spec.channels == 1 || spec.channels == 2)) {
      selected = &spec;
      break;
    }
#endif
    if (spec.sample_rate_hz != 8000 || spec.channels != 1) continue;
    if (spec.codec_name == "PCMU") {
      selected = &spec;
      break;
    }
    if (!selected && spec.codec_name == "PCMA") {
      selected = &spec;
    }
  }
  if (!selected) {
    engine->audio_outbound_last_error =
      "remote SDP did not negotiate a supported outbound audio payload";
    return false;
  }

  engine->audio_outbound_payload_type = selected->payload_type;
  engine->audio_outbound_sample_rate_hz = selected->sample_rate_hz;
  engine->audio_outbound_channels = selected->channels;
  engine->audio_outbound_codec_name = selected->codec_name;
  engine->audio_outbound_last_error.clear();
  return true;
}

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

std::array<unsigned char, kRtcpSenderReportBytes> build_rtcp_sender_report(
  const EmbeddedTransportState& engine,
  uint32_t rtp_timestamp
) {
  std::array<unsigned char, kRtcpSenderReportBytes> out{};
  const auto ntp = current_ntp_timestamp_words();
  out[0] = 0x80u;
  out[1] = kRtcpPacketTypeSenderReport;
  write_u16_be(out.data() + 2, 6);
  write_u32_be(out.data() + 4, engine.outbound_rtp_ssrc);
  write_u32_be(out.data() + 8, ntp.first);
  write_u32_be(out.data() + 12, ntp.second);
  write_u32_be(out.data() + 16, rtp_timestamp);
  write_u32_be(out.data() + 20, static_cast<uint32_t>(engine.rtp_packets_sent));
  write_u32_be(out.data() + 24, static_cast<uint32_t>(engine.rtp_payload_bytes_sent));
  return out;
}

uint8_t encode_pcmu_sample(int16_t sample) {
  constexpr int kBias = 0x84;
  constexpr int kClip = 32635;
  static constexpr std::array<int, 8> kSegmentEnd = {{
    0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF,
  }};

  int value = static_cast<int>(sample);
  uint8_t mask = 0xFFu;
  if (value < 0) {
    value = -value;
    mask = 0x7Fu;
  }
  value = std::min(value, kClip) + kBias;

  int segment = 0;
  while (segment < static_cast<int>(kSegmentEnd.size()) && value > kSegmentEnd[segment]) {
    segment += 1;
  }
  if (segment >= static_cast<int>(kSegmentEnd.size())) {
    return static_cast<uint8_t>(0x7Fu ^ mask);
  }
  const uint8_t encoded = static_cast<uint8_t>(
    (segment << 4) | ((value >> (segment + 3)) & 0x0F));
  return static_cast<uint8_t>(encoded ^ mask);
}

uint8_t encode_pcma_sample(int16_t sample) {
  static constexpr std::array<int, 8> kSegmentEnd = {{
    0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
  }};

  int value = static_cast<int>(sample);
  uint8_t mask = 0xD5u;
  if (value < 0) {
    value = -value - 1;
    mask = 0x55u;
  }
  value = std::min(value, 32635);

  int segment = 0;
  while (segment < static_cast<int>(kSegmentEnd.size()) && value > kSegmentEnd[segment]) {
    segment += 1;
  }
  if (segment >= static_cast<int>(kSegmentEnd.size())) {
    return static_cast<uint8_t>(0x7Fu ^ mask);
  }

  uint8_t encoded = static_cast<uint8_t>(segment << 4);
  encoded |= static_cast<uint8_t>(
    segment < 2 ? ((value >> 4) & 0x0F) : ((value >> (segment + 3)) & 0x0F));
  return static_cast<uint8_t>(encoded ^ mask);
}

std::vector<unsigned char> encode_pcm16_to_g711_20ms(
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  const std::string& codec_name
) {
  const bool use_pcma = codec_name == "PCMA";
  std::vector<unsigned char> payload(
    kOutboundG711FrameSamples,
    use_pcma ? encode_pcma_sample(0) : encode_pcmu_sample(0));
  if (!pcm || pcm_samples == 0 || sample_rate_hz <= 0 || channels <= 0) return payload;

  const size_t channels_sz = static_cast<size_t>(channels);
  const size_t frame_count = pcm_samples / channels_sz;
  if (frame_count == 0) return payload;
  for (size_t i = 0; i < payload.size(); ++i) {
    size_t source_frame =
      (static_cast<uint64_t>(i) * static_cast<uint64_t>(sample_rate_hz)) / 8000u;
    if (source_frame >= frame_count) source_frame = frame_count - 1;
    payload[i] = use_pcma
      ? encode_pcma_sample(pcm[source_frame * channels_sz])
      : encode_pcmu_sample(pcm[source_frame * channels_sz]);
  }
  return payload;
}

std::vector<int16_t> resample_interleaved_pcm16_20ms(
  const int16_t* pcm,
  size_t pcm_samples,
  int source_sample_rate_hz,
  int source_channels,
  int target_sample_rate_hz,
  int target_channels,
  size_t target_samples_per_channel
) {
  std::vector<int16_t> out(target_samples_per_channel * static_cast<size_t>(target_channels), 0);
  if (!pcm || pcm_samples == 0 ||
      source_sample_rate_hz <= 0 || source_channels <= 0 ||
      target_sample_rate_hz <= 0 || target_channels <= 0 ||
      target_samples_per_channel == 0) {
    return out;
  }

  const size_t source_channels_sz = static_cast<size_t>(source_channels);
  const size_t target_channels_sz = static_cast<size_t>(target_channels);
  const size_t source_frames = pcm_samples / source_channels_sz;
  if (source_frames == 0) return out;

  for (size_t frame = 0; frame < target_samples_per_channel; ++frame) {
    size_t source_frame =
      (static_cast<uint64_t>(frame) * static_cast<uint64_t>(source_sample_rate_hz)) /
      static_cast<uint64_t>(target_sample_rate_hz);
    if (source_frame >= source_frames) source_frame = source_frames - 1;
    for (size_t channel = 0; channel < target_channels_sz; ++channel) {
      const size_t source_channel =
        std::min(channel, source_channels_sz - static_cast<size_t>(1));
      out[frame * target_channels_sz + channel] =
        pcm[source_frame * source_channels_sz + source_channel];
    }
  }
  return out;
}

#if defined(AGENTD_HAVE_OPUS)
bool ensure_outbound_opus_encoder(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing embedded transport state";
    return false;
  }
  if (engine->audio_outbound_sample_rate_hz != 48000 ||
      (engine->audio_outbound_channels != 1 && engine->audio_outbound_channels != 2)) {
    if (out_err) *out_err = "unsupported outbound Opus RTP format";
    return false;
  }
  const int sample_rate_hz = static_cast<int>(engine->audio_outbound_sample_rate_hz);
  const int channels = static_cast<int>(engine->audio_outbound_channels);
  if (engine->outbound_opus_encoder &&
      engine->outbound_opus_sample_rate_hz == sample_rate_hz &&
      engine->outbound_opus_channels == channels) {
    return true;
  }
  if (engine->outbound_opus_encoder) {
    opus_encoder_destroy(engine->outbound_opus_encoder);
    engine->outbound_opus_encoder = nullptr;
    engine->outbound_opus_sample_rate_hz = 0;
    engine->outbound_opus_channels = 0;
  }

  int opus_err = OPUS_OK;
  engine->outbound_opus_encoder =
    opus_encoder_create(sample_rate_hz, channels, OPUS_APPLICATION_AUDIO, &opus_err);
  if (!engine->outbound_opus_encoder) {
    if (out_err) *out_err = std::string("opus encoder creation failed: ") + opus_strerror(opus_err);
    return false;
  }
  engine->outbound_opus_sample_rate_hz = sample_rate_hz;
  engine->outbound_opus_channels = channels;
  return true;
}

bool encode_pcm16_to_opus_20ms(
  EmbeddedTransportState* engine,
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  std::vector<unsigned char>* out_payload,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_payload) out_payload->clear();
  if (!out_payload) {
    if (out_err) *out_err = "missing Opus payload output";
    return false;
  }
  if (!ensure_outbound_opus_encoder(engine, out_err)) return false;

  const int target_sample_rate_hz = static_cast<int>(engine->audio_outbound_sample_rate_hz);
  const int target_channels = static_cast<int>(engine->audio_outbound_channels);
  const std::vector<int16_t> opus_pcm = resample_interleaved_pcm16_20ms(
    pcm,
    pcm_samples,
    sample_rate_hz,
    channels,
    target_sample_rate_hz,
    target_channels,
    kOutboundOpusFrameSamplesPerChannel);

  std::array<unsigned char, kOutboundOpusMaxPayloadBytes> encoded{};
  const int encoded_bytes = opus_encode(
    engine->outbound_opus_encoder,
    reinterpret_cast<const opus_int16*>(opus_pcm.data()),
    static_cast<int>(kOutboundOpusFrameSamplesPerChannel),
    encoded.data(),
    static_cast<opus_int32>(encoded.size()));
  if (encoded_bytes < 0) {
    if (out_err) *out_err = std::string("opus encode failed: ") + opus_strerror(encoded_bytes);
    return false;
  }
  if (encoded_bytes == 0) {
    if (out_err) *out_err = "opus encode returned an empty payload";
    return false;
  }
  out_payload->assign(encoded.data(), encoded.data() + encoded_bytes);
  return true;
}
#endif

bool encode_outbound_audio_payload_20ms(
  EmbeddedTransportState* engine,
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  std::vector<unsigned char>* out_payload,
  uint32_t* out_timestamp_increment,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_payload) out_payload->clear();
  if (out_timestamp_increment) *out_timestamp_increment = 0;
  if (!engine || !out_payload || !out_timestamp_increment) {
    if (out_err) *out_err = "missing outbound audio encode arguments";
    return false;
  }

  if (engine->audio_outbound_codec_name == "PCMU" ||
      engine->audio_outbound_codec_name == "PCMA") {
    *out_payload = encode_pcm16_to_g711_20ms(
      pcm, pcm_samples, sample_rate_hz, channels, engine->audio_outbound_codec_name);
    *out_timestamp_increment = static_cast<uint32_t>(out_payload->size());
    return true;
  }

#if defined(AGENTD_HAVE_OPUS)
  if (engine->audio_outbound_codec_name == "OPUS") {
    if (!encode_pcm16_to_opus_20ms(
          engine, pcm, pcm_samples, sample_rate_hz, channels, out_payload, out_err)) {
      return false;
    }
    *out_timestamp_increment = static_cast<uint32_t>(kOutboundOpusFrameSamplesPerChannel);
    return true;
  }
#endif

  if (out_err) *out_err = "outbound audio payload codec was not negotiated";
  return false;
}

bool transmit_outbound_rtcp_sender_report(
  EmbeddedTransportState* engine,
  uint32_t rtp_timestamp,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!engine || !engine->agent || !engine->outbound_srtp || !engine->srtp_outbound_ready) {
    if (out_err) *out_err = "outbound SRTCP transport not ready";
    return false;
  }

  const std::array<unsigned char, kRtcpSenderReportBytes> plain =
    build_rtcp_sender_report(*engine, rtp_timestamp);
  std::vector<unsigned char> protected_packet;
  std::string protect_err;
  if (!agentd::protect_outbound_rtcp_packet(
        engine->outbound_srtp,
        plain.data(),
        plain.size(),
        &protected_packet,
        &protect_err)) {
    if (out_err) *out_err = protect_err;
    return false;
  }
  const int rc = juice_send(
    engine->agent,
    reinterpret_cast<const char*>(protected_packet.data()),
    protected_packet.size());
  if (rc != JUICE_ERR_SUCCESS) {
    if (out_err) *out_err = "libjuice RTCP send failed with code " + std::to_string(rc);
    return false;
  }

  engine->rtcp_packets_sent += 1;
  engine->rtcp_payload_bytes_sent += plain.size();
  engine->rtcp_last_sent_packet_type = kRtcpPacketTypeSenderReport;
  engine->rtcp_last_sent_ssrc = engine->outbound_rtp_ssrc;
  engine->rtcp_last_error.clear();
  return true;
}

bool transmit_outbound_audio_rtp(
  EmbeddedTransportState* engine,
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!engine || !pcm || pcm_samples == 0) {
    if (out_err) *out_err = "missing outbound audio samples";
    return false;
  }
  if (!engine->agent || !engine->srtp_outbound_ready || !engine->outbound_srtp) {
    if (out_err) *out_err = "outbound SRTP transport not ready";
    return false;
  }
  if (sample_rate_hz <= 0 || channels <= 0) {
    if (out_err) *out_err = "outbound audio format unavailable";
    return false;
  }
  if (engine->audio_outbound_payload_type < 0 ||
      engine->audio_outbound_codec_name.empty()) {
    if (out_err) *out_err = "outbound audio payload was not negotiated";
    return false;
  }

  std::vector<unsigned char> payload;
  uint32_t timestamp_increment = 0;
  if (!encode_outbound_audio_payload_20ms(
        engine,
        pcm,
        pcm_samples,
        sample_rate_hz,
        channels,
        &payload,
        &timestamp_increment,
        out_err)) {
    return false;
  }
  std::vector<unsigned char> plain(12 + payload.size());
  plain[0] = 0x80u;
  plain[1] = static_cast<unsigned char>(engine->audio_outbound_payload_type);
  const uint16_t sequence = engine->outbound_rtp_sequence++;
  const uint32_t timestamp = engine->outbound_rtp_timestamp;
  engine->outbound_rtp_timestamp += timestamp_increment;
  write_u16_be(plain.data() + 2, sequence);
  write_u32_be(plain.data() + 4, timestamp);
  write_u32_be(plain.data() + 8, engine->outbound_rtp_ssrc);
  std::memcpy(plain.data() + 12, payload.data(), payload.size());

  std::vector<unsigned char> protected_packet;
  std::string protect_err;
  if (!agentd::protect_outbound_rtp_packet(
        engine->outbound_srtp,
        plain.data(),
        plain.size(),
        &protected_packet,
        &protect_err)) {
    if (out_err) *out_err = protect_err;
    return false;
  }
  const int rc = juice_send(
    engine->agent,
    reinterpret_cast<const char*>(protected_packet.data()),
    protected_packet.size());
  if (rc != JUICE_ERR_SUCCESS) {
    if (out_err) *out_err = "libjuice RTP send failed with code " + std::to_string(rc);
    return false;
  }

  engine->rtp_packets_sent += 1;
  engine->rtp_payload_bytes_sent += payload.size();
  engine->rtp_last_sent_payload_type = engine->audio_outbound_payload_type;
  engine->rtp_last_sent_sequence = sequence;
  engine->rtp_last_sent_timestamp = timestamp;
  engine->rtp_last_sent_ssrc = engine->outbound_rtp_ssrc;
  engine->audio_outbound_frames_sent += 1;
  engine->audio_pcm_samples_submitted_total += pcm_samples;
  engine->audio_last_outbound_samples = timestamp_increment;
  engine->audio_outbound_last_error.clear();
  std::string rtcp_err;
  if (!transmit_outbound_rtcp_sender_report(engine, timestamp, &rtcp_err)) {
    engine->rtcp_last_error =
      rtcp_err.empty() ? std::string("outbound RTCP sender report transmit failed") : rtcp_err;
  }
  return true;
}

bool ingest_inbound_srtp_packet(
  EmbeddedTransportState* engine,
  const char* data,
  size_t size,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!engine || !data || size == 0) {
    if (out_err) *out_err = "missing inbound media packet";
    return false;
  }
  if (!engine->srtp_inbound_ready || !engine->inbound_srtp) {
    if (out_err) *out_err = "inbound SRTP context not ready";
    return false;
  }

  std::vector<unsigned char> packet(
    reinterpret_cast<const unsigned char*>(data),
    reinterpret_cast<const unsigned char*>(data) + size);
  int packet_len = static_cast<int>(packet.size());
  if (packet_len <= 0) {
    if (out_err) *out_err = "empty inbound media packet";
    return false;
  }

  agentd::ParsedRtpPacketInfo rtp_info;
  agentd::ParsedRtcpPacketInfo rtcp_info;
  bool was_rtcp = false;
  std::string ingest_err;
  if (!agentd::unprotect_inbound_srtp_packet(
        engine->inbound_srtp,
        packet.data(),
        static_cast<size_t>(packet_len),
        &rtp_info,
        &was_rtcp,
        &ingest_err,
        &rtcp_info)) {
    engine->srtp_last_error = ingest_err;
    if (out_err) *out_err = ingest_err;
    return false;
  }
  engine->srtp_last_error.clear();

  if (was_rtcp) {
    engine->rtcp_packets_received += 1;
    engine->rtcp_payload_bytes_received += rtcp_info.packet_size;
    engine->rtcp_last_packet_type = rtcp_info.packet_type;
    engine->rtcp_last_ssrc = rtcp_info.ssrc;
    engine->rtcp_last_error.clear();
    return true;
  }

  engine->rtp_packets_received += 1;
  engine->rtp_payload_bytes_received += rtp_info.payload_size;
  engine->rtp_last_payload_type = rtp_info.payload_type;
  engine->rtp_last_sequence = rtp_info.sequence;
  engine->rtp_last_timestamp = rtp_info.timestamp;
  engine->rtp_last_ssrc = rtp_info.ssrc;
  if (rtp_info.payload_size > 0) {
    (void)decode_inbound_audio_frame(
      engine,
      rtp_info.payload_type,
      packet.data() + rtp_info.payload_offset,
      rtp_info.payload_size,
      nullptr);
  }
  return true;
}

bool drain_dtls_outbound(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine || !engine->dtls_ssl || !engine->agent) return true;
  BIO* wbio = SSL_get_wbio(engine->dtls_ssl);
  if (!wbio) return true;
  char packet[2048];
  for (;;) {
    const int n = BIO_read(wbio, packet, sizeof(packet));
    if (n <= 0) break;
    const int rc = juice_send(engine->agent, packet, static_cast<size_t>(n));
    if (rc != JUICE_ERR_SUCCESS) {
      const std::string err = "libjuice send failed with code " + std::to_string(rc);
      mark_dtls_failure(engine, err);
      if (out_err) *out_err = err;
      return false;
    }
    engine->dtls_packets_sent += 1;
  }
  return true;
}

bool advance_dtls_handshake(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine || !engine->dtls_ssl) return true;
  if (engine->dtls_handshake_ready) return true;
  if (engine->dtls_handshake_state == "failed") {
    if (out_err && !engine->dtls_last_error.empty()) *out_err = engine->dtls_last_error;
    return false;
  }
  if (engine->dtls_handshake_state.empty() || engine->dtls_handshake_state == "idle") {
    engine->dtls_handshake_state = "handshaking";
  }

  const int rc = SSL_do_handshake(engine->dtls_ssl);
  std::string send_err;
  if (!drain_dtls_outbound(engine, &send_err)) {
    if (out_err) *out_err = send_err;
    return false;
  }
  if (rc == 1) {
    engine->dtls_handshake_ready = true;
    engine->dtls_handshake_state = "connected";
    engine->dtls_last_error.clear();
    engine->dtls_selected_srtp_profile = selected_dtls_srtp_profile_name(engine->dtls_ssl);
    if (engine->dtls_selected_srtp_profile.empty()) {
      if (out_err) *out_err = "openssl DTLS-SRTP profile negotiation failed";
      return true;
    }
    std::string srtp_err;
    if (!ensure_srtp_contexts(engine, &srtp_err) && out_err && !srtp_err.empty()) {
      *out_err = srtp_err;
    }
    return true;
  }

  const int ssl_err = SSL_get_error(engine->dtls_ssl, rc);
  if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
    engine->dtls_handshake_state = "handshaking";
    engine->dtls_last_error.clear();
    return true;
  }

  const std::string err = ssl_error_text(engine->dtls_ssl, rc);
  mark_dtls_failure(engine, err);
  if (out_err) *out_err = err;
  return false;
}

bool ensure_dtls_transport(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->dtls_ssl) return true;
  if (!engine->dtls_identity_ready && !generate_dtls_identity(engine, out_err)) return false;

  using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
  using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;
  using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

  SslCtxPtr ctx(SSL_CTX_new(DTLS_server_method()), SSL_CTX_free);
  if (!ctx) {
    if (out_err) *out_err = "openssl dtls ctx allocation failed: " + openssl_last_error_text();
    return false;
  }
  if (SSL_CTX_set_min_proto_version(ctx.get(), DTLS1_2_VERSION) != 1) {
    if (out_err) *out_err = "openssl dtls min proto init failed: " + openssl_last_error_text();
    return false;
  }
  SSL_CTX_set_read_ahead(ctx.get(), 1);
  if (SSL_CTX_use_certificate(ctx.get(), engine->dtls_certificate) != 1 ||
      SSL_CTX_use_PrivateKey(ctx.get(), engine->dtls_private_key) != 1 ||
      SSL_CTX_check_private_key(ctx.get()) != 1) {
    if (out_err) *out_err = "openssl dtls cert/key install failed: " + openssl_last_error_text();
    return false;
  }
  if (SSL_CTX_set_tlsext_use_srtp(ctx.get(), kDtlsSrtpProfiles) != 0) {
    if (out_err) *out_err = "openssl dtls SRTP profile config failed: " + openssl_last_error_text();
    return false;
  }

  SslPtr ssl(SSL_new(ctx.get()), SSL_free);
  if (!ssl) {
    if (out_err) *out_err = "openssl dtls ssl allocation failed: " + openssl_last_error_text();
    return false;
  }
  BioPtr rbio(BIO_new(BIO_s_dgram_mem()), BIO_free);
  BioPtr wbio(BIO_new(BIO_s_dgram_mem()), BIO_free);
  if (!rbio || !wbio) {
    if (out_err) *out_err = "openssl dtls bio allocation failed: " + openssl_last_error_text();
    return false;
  }
  (void)BIO_ctrl(rbio.get(), BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  (void)BIO_ctrl(wbio.get(), BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  SSL_set_bio(ssl.get(), rbio.release(), wbio.release());
  SSL_set_options(ssl.get(), SSL_OP_NO_QUERY_MTU);
  SSL_set_mtu(ssl.get(), kDtlsDatagramMtu);
  SSL_set_accept_state(ssl.get());

  engine->dtls_ctx = ctx.release();
  engine->dtls_ssl = ssl.release();
  engine->dtls_handshake_state = "ready_for_client_hello";
  engine->dtls_last_error.clear();
  return true;
}

std::string build_answer_sdp(
  const std::string& remote_sdp,
  const EmbeddedTransportState& state
) {
  const std::vector<std::string> remote_lines = split_sdp_lines(remote_sdp);
  if (!contains_media_section(remote_lines) || !state.dtls_identity_ready) {
    return state.local_description;
  }

  const std::vector<std::string> local_ice_lines = local_ice_lines_from_description(state.local_description);
  std::vector<std::string> out;
  out.push_back("v=0");
  out.push_back("o=- 0 0 IN IP4 127.0.0.1");
  out.push_back("s=-");
  out.push_back("t=0 0");
  for (const auto& line : remote_lines) {
    if (should_copy_session_attribute(line)) out.push_back(line);
  }

  bool in_media = false;
  std::vector<std::string> media_lines;
  auto flush_media = [&]() {
    if (media_lines.empty()) return;
    out.push_back(rewrite_mline_for_inactive_answer(media_lines.front()));
    out.push_back("c=IN IP4 0.0.0.0");
    for (size_t i = 1; i < media_lines.size(); ++i) {
      if (should_copy_media_attribute(media_lines[i])) out.push_back(media_lines[i]);
    }
    out.push_back("a=inactive");
    out.push_back("a=setup:" + state.dtls_setup_role);
    out.push_back("a=fingerprint:sha-256 " + state.dtls_fingerprint_sha256);
    for (const auto& line : local_ice_lines) out.push_back(line);
    media_lines.clear();
  };

  for (const auto& line : remote_lines) {
    if (starts_with(line, "m=")) {
      if (in_media) flush_media();
      in_media = true;
      media_lines.push_back(line);
      continue;
    }
    if (in_media) media_lines.push_back(line);
  }
  flush_media();
  return join_sdp_lines(out);
}

bool refresh_local_description(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine || !engine->agent) {
    if (out_err) *out_err = "missing libjuice agent";
    return false;
  }
  char description[JUICE_MAX_SDP_STRING_LEN];
  std::memset(description, 0, sizeof(description));
  if (juice_get_local_description(engine->agent, description, sizeof(description)) != JUICE_ERR_SUCCESS) {
    if (out_err) *out_err = "libjuice local description generation failed";
    return false;
  }
  engine->local_description = description;
  return true;
}

void refresh_transport_snapshot(EmbeddedTransportState* engine) {
  if (!engine || !engine->agent) return;

  const juice_state_t state = juice_get_state(engine->agent);
  engine->libjuice_state = juice_state_text(state);

  char local_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  char remote_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  std::memset(local_candidate, 0, sizeof(local_candidate));
  std::memset(remote_candidate, 0, sizeof(remote_candidate));
  if (juice_get_selected_candidates(
        engine->agent,
        local_candidate,
        sizeof(local_candidate),
        remote_candidate,
        sizeof(remote_candidate)) == JUICE_ERR_SUCCESS) {
    engine->selected_local_candidate = local_candidate;
    engine->selected_remote_candidate = remote_candidate;
    engine->transport_connectivity_ready = true;
  }

  char local_address[JUICE_MAX_ADDRESS_STRING_LEN];
  char remote_address[JUICE_MAX_ADDRESS_STRING_LEN];
  std::memset(local_address, 0, sizeof(local_address));
  std::memset(remote_address, 0, sizeof(remote_address));
  if (juice_get_selected_addresses(
        engine->agent,
        local_address,
        sizeof(local_address),
        remote_address,
        sizeof(remote_address)) == JUICE_ERR_SUCCESS) {
    engine->selected_local_address = local_address;
    engine->selected_remote_address = remote_address;
    engine->transport_connectivity_ready = true;
  }

  if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
    engine->transport_connectivity_ready = true;
  }
}

EmbeddedTransportState::AsyncProgressKey capture_async_progress_key(
  const EmbeddedTransportState& state
) {
  EmbeddedTransportState::AsyncProgressKey key;
  key.libjuice_state = state.libjuice_state;
  key.dtls_handshake_state = state.dtls_handshake_state;
  key.dtls_selected_srtp_profile = state.dtls_selected_srtp_profile;
  key.srtp_last_error = state.srtp_last_error;
  key.last_remote_description_error = state.last_remote_description_error;
  key.selected_local_candidate = state.selected_local_candidate;
  key.selected_remote_candidate = state.selected_remote_candidate;
  key.selected_local_address = state.selected_local_address;
  key.selected_remote_address = state.selected_remote_address;
  key.offers_seen = state.offers_seen;
  key.remote_candidates_seen = state.remote_candidates_seen;
  key.local_candidates_observed = state.local_candidates_observed;
  key.gathering_done = state.gathering_done;
  key.gather_started = state.gather_started;
  key.remote_description_applied = state.remote_description_applied;
  key.transport_connectivity_ready = state.transport_connectivity_ready;
  key.dtls_identity_ready = state.dtls_identity_ready;
  key.dtls_handshake_ready = state.dtls_handshake_ready;
  key.dtls_exporter_ready = state.dtls_exporter_ready;
  key.srtp_contexts_ready = state.srtp_contexts_ready;
  key.srtp_inbound_ready = state.srtp_inbound_ready;
  key.srtp_outbound_ready = state.srtp_outbound_ready;
  key.rtp_packets_received = state.rtp_packets_received;
  key.rtp_payload_bytes_received = state.rtp_payload_bytes_received;
  key.rtp_packets_sent = state.rtp_packets_sent;
  key.rtp_payload_bytes_sent = state.rtp_payload_bytes_sent;
  key.rtcp_packets_received = state.rtcp_packets_received;
  key.rtcp_packets_sent = state.rtcp_packets_sent;
  key.rtcp_payload_bytes_received = state.rtcp_payload_bytes_received;
  key.rtcp_payload_bytes_sent = state.rtcp_payload_bytes_sent;
  key.rtp_last_payload_type = state.rtp_last_payload_type;
  key.rtp_last_sequence = state.rtp_last_sequence;
  key.rtp_last_timestamp = state.rtp_last_timestamp;
  key.rtp_last_ssrc = state.rtp_last_ssrc;
  key.rtp_last_sent_payload_type = state.rtp_last_sent_payload_type;
  key.rtp_last_sent_sequence = state.rtp_last_sent_sequence;
  key.rtp_last_sent_timestamp = state.rtp_last_sent_timestamp;
  key.rtp_last_sent_ssrc = state.rtp_last_sent_ssrc;
  key.rtcp_last_packet_type = state.rtcp_last_packet_type;
  key.rtcp_last_ssrc = state.rtcp_last_ssrc;
  key.rtcp_last_sent_packet_type = state.rtcp_last_sent_packet_type;
  key.rtcp_last_sent_ssrc = state.rtcp_last_sent_ssrc;
  key.audio_frames_decoded = state.audio_frames_decoded;
  key.audio_pcm_samples_decoded = state.audio_pcm_samples_decoded;
  key.audio_pcm_samples_buffered = state.audio_pcm_samples_buffered;
  key.audio_outbound_frames_sent = state.audio_outbound_frames_sent;
  key.audio_pcm_samples_submitted_total = state.audio_pcm_samples_submitted_total;
  key.audio_last_outbound_samples = state.audio_last_outbound_samples;
  key.audio_outbound_payload_type = state.audio_outbound_payload_type;
  key.audio_outbound_sample_rate_hz = state.audio_outbound_sample_rate_hz;
  key.audio_outbound_channels = state.audio_outbound_channels;
  key.audio_outbound_codec_name = state.audio_outbound_codec_name;
  key.audio_last_sample_rate_hz = state.audio_last_sample_rate_hz;
  key.audio_last_channels = state.audio_last_channels;
  key.audio_last_frame_samples_per_channel = state.audio_last_frame_samples_per_channel;
  key.audio_last_codec_name = state.audio_last_codec_name;
  key.audio_last_error = state.audio_last_error;
  key.audio_outbound_last_error = state.audio_outbound_last_error;
  key.rtcp_last_error = state.rtcp_last_error;
  return key;
}

std::string derived_media_engine_state(const EmbeddedTransportState& state) {
  if (state.dtls_handshake_state == "failed") return "failed";
  if (state.rtp_packets_received > 0 || state.rtp_packets_sent > 0 ||
      state.rtcp_packets_received > 0 || state.rtcp_packets_sent > 0) {
    return "media_active";
  }
  if (state.srtp_contexts_ready) return "media_transport_ready";
  if (state.dtls_handshake_ready) return "dtls_connected";
  if (state.transport_connectivity_ready) return "transport_connected";
  if (state.remote_description_applied) return "signaling_active";
  return "signaling_ready";
}

void push_async_event_json(EmbeddedTransportState* engine, const std::string& payload) {
  if (!engine || payload.empty()) return;
  std::lock_guard<std::mutex> lk(engine->async_events_mu);
  engine->pending_async_events.push_back(payload);
}

void sync_async_progress_baseline(EmbeddedTransportState* engine) {
  if (!engine) return;
  std::lock_guard<std::mutex> lk(engine->async_events_mu);
  engine->last_async_key = capture_async_progress_key(*engine);
  engine->last_async_key_initialized = true;
  engine->pending_async_events.clear();
}

void maybe_enqueue_progress_event(
  EmbeddedTransportState* engine,
  const std::string& event_name
) {
  if (!engine) return;
  const EmbeddedTransportState::AsyncProgressKey current = capture_async_progress_key(*engine);
  {
    std::lock_guard<std::mutex> lk(engine->async_events_mu);
    if (engine->last_async_key_initialized && current == engine->last_async_key) {
      return;
    }
    engine->last_async_key = current;
    engine->last_async_key_initialized = true;
  }
  const std::string payload = build_event_json(
    *engine,
    event_name.empty() ? std::string("embedded_transport_progress") : event_name,
    derived_media_engine_state(*engine),
    0);
  push_async_event_json(engine, payload);
}

bool pop_async_event_json(EmbeddedTransportState* engine, std::string* out_payload) {
  if (out_payload) out_payload->clear();
  if (!engine) return false;
  std::lock_guard<std::mutex> lk(engine->async_events_mu);
  if (engine->pending_async_events.empty()) return false;
  if (out_payload) *out_payload = engine->pending_async_events.front();
  engine->pending_async_events.pop_front();
  return true;
}

void wait_for_local_gathering(EmbeddedTransportState* engine, int timeout_ms) {
  if (!engine || !engine->agent || timeout_ms <= 0) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    refresh_transport_snapshot(engine);
    if (engine->gathering_done || engine->local_candidates_observed > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void wait_for_transport_progress(EmbeddedTransportState* engine, int timeout_ms) {
  if (!engine || !engine->agent || timeout_ms <= 0) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    refresh_transport_snapshot(engine);
    if (engine->transport_connectivity_ready) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

bool reserve_transport_runtime(std::string* out_err) {
  if (out_err) out_err->clear();
  std::lock_guard<std::mutex> lk(g_transport_runtime_mu);
  if (g_transport_runtime_refs == 0) {
    const srtp_err_status_t srtp_status = srtp_init();
    if (srtp_status != srtp_err_status_ok) {
      if (out_err) {
        *out_err = "libsrtp initialization failed with status " +
                   std::to_string(static_cast<int>(srtp_status));
      }
      return false;
    }
    g_srtp_initialized = true;
    usrsctp_init(0, nullptr, nullptr);
    g_usrsctp_initialized = true;
  }
  g_transport_runtime_refs += 1;
  return true;
}

void release_transport_runtime() {
  std::lock_guard<std::mutex> lk(g_transport_runtime_mu);
  if (g_transport_runtime_refs == 0) return;
  g_transport_runtime_refs -= 1;
  if (g_transport_runtime_refs != 0) return;
  if (g_usrsctp_initialized) {
    (void)usrsctp_finish();
    g_usrsctp_initialized = false;
  }
  if (g_srtp_initialized) {
    (void)srtp_shutdown();
    g_srtp_initialized = false;
  }
}

void on_state_changed(juice_agent_t*, juice_state_t state, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->libjuice_state = juice_state_text(state);
  refresh_transport_snapshot(engine);
  maybe_enqueue_progress_event(engine, "embedded_transport_progress");
}

void on_candidate(juice_agent_t*, const char*, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->local_candidates_observed += 1;
}

void on_gathering_done(juice_agent_t*, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->gathering_done = true;
  refresh_transport_snapshot(engine);
  maybe_enqueue_progress_event(engine, "embedded_transport_progress");
}

void on_recv(juice_agent_t*, const char* data, size_t size, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine || !engine->dtls_ssl || !data || size == 0) return;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  std::string err;
  if (agentd::is_probable_dtls_packet(bytes, size)) {
    BIO* rbio = SSL_get_rbio(engine->dtls_ssl);
    if (!rbio) return;
    const int wrote = BIO_write(rbio, data, static_cast<int>(size));
    if (wrote <= 0) {
      mark_dtls_failure(engine, "openssl dtls inbound packet write failed: " + openssl_last_error_text());
      return;
    }
    engine->dtls_packets_received += 1;
    if (engine->dtls_handshake_state == "ready_for_client_hello") {
      engine->dtls_handshake_state = "handshaking";
    }
    (void)advance_dtls_handshake(engine, &err);
  } else if (agentd::is_probable_rtp_or_rtcp_packet(bytes, size)) {
    (void)ingest_inbound_srtp_packet(engine, data, size, &err);
  }
  refresh_transport_snapshot(engine);
  maybe_enqueue_progress_event(engine, "embedded_transport_progress");
}

bool ensure_agent(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->agent) return true;
  if (!generate_dtls_identity(engine, out_err)) return false;
  if (!ensure_dtls_transport(engine, out_err)) return false;

  juice_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  cfg.cb_state_changed = &on_state_changed;
  cfg.cb_candidate = &on_candidate;
  cfg.cb_gathering_done = &on_gathering_done;
  cfg.cb_recv = &on_recv;
  cfg.user_ptr = engine;

  engine->agent = juice_create(&cfg);
  if (!engine->agent) {
    if (out_err) *out_err = "libjuice agent creation failed";
    return false;
  }
  engine->libjuice_state = juice_state_text(juice_get_state(engine->agent));

  if (juice_gather_candidates(engine->agent) != JUICE_ERR_SUCCESS) {
    juice_destroy(engine->agent);
    engine->agent = nullptr;
    if (out_err) *out_err = "libjuice candidate gathering failed";
    return false;
  }
  engine->gather_started = true;
  wait_for_local_gathering(engine, 500);
  if (!refresh_local_description(engine, out_err)) {
    juice_destroy(engine->agent);
    engine->agent = nullptr;
    return false;
  }
  refresh_transport_snapshot(engine);
  return true;
}

std::string build_event_json(
  const EmbeddedTransportState& state,
  const std::string& event_name,
  const std::string& media_engine_state,
  uint64_t initial_remote_candidate_count
) {
  const char* srtp_version = srtp_get_version_string();
  std::string json =
    std::string("{\"ok\":true,\"event\":\"") + json_escape(event_name) +
    "\",\"media_engine_state\":\"" + json_escape(media_engine_state) +
    "\",\"media_engine_kind\":\"builtin_native_plugin\""
    ",\"native_media_supported\":true"
    ",\"native_media_active\":" +
    std::string((state.rtp_packets_received > 0 || state.rtp_packets_sent > 0 ||
                 state.rtcp_packets_received > 0 || state.rtcp_packets_sent > 0) ? "true" : "false") +
    ",\"provider\":\"" + std::string(kProviderName) + "\""
    ",\"transport_family\":\"embedded_transport_primitives\""
    ",\"dtls_identity_ready\":" + std::string(state.dtls_identity_ready ? "true" : "false") +
    ",\"dtls_handshake_ready\":" + std::string(state.dtls_handshake_ready ? "true" : "false") +
    ",\"dtls_exporter_ready\":" + std::string(state.dtls_exporter_ready ? "true" : "false") +
    ",\"srtp_contexts_ready\":" + std::string(state.srtp_contexts_ready ? "true" : "false") +
    ",\"srtp_inbound_ready\":" + std::string(state.srtp_inbound_ready ? "true" : "false") +
    ",\"srtp_outbound_ready\":" + std::string(state.srtp_outbound_ready ? "true" : "false") +
    ",\"dtls_setup_role\":\"" + json_escape(state.dtls_setup_role) + "\""
    ",\"dtls_handshake_state\":\"" + json_escape(state.dtls_handshake_state) + "\""
    ",\"sdp_answer_shape\":\"" + json_escape(state.last_answer_sdp_shape) + "\""
    ",\"libjuice_state\":\"" + json_escape(state.libjuice_state) + "\""
    ",\"libjuice_local_description_bytes\":" + std::to_string(state.local_description.size()) +
    ",\"dtls_packets_sent\":" + std::to_string(state.dtls_packets_sent) +
    ",\"dtls_packets_received\":" + std::to_string(state.dtls_packets_received) +
    ",\"rtp_packets_received\":" + std::to_string(state.rtp_packets_received) +
    ",\"rtp_payload_bytes_received\":" + std::to_string(state.rtp_payload_bytes_received) +
    ",\"rtp_packets_sent\":" + std::to_string(state.rtp_packets_sent) +
    ",\"rtp_payload_bytes_sent\":" + std::to_string(state.rtp_payload_bytes_sent) +
    ",\"rtcp_packets_received\":" + std::to_string(state.rtcp_packets_received) +
    ",\"rtcp_packets_sent\":" + std::to_string(state.rtcp_packets_sent) +
    ",\"rtcp_payload_bytes_received\":" + std::to_string(state.rtcp_payload_bytes_received) +
    ",\"rtcp_payload_bytes_sent\":" + std::to_string(state.rtcp_payload_bytes_sent) +
    ",\"audio_frames_decoded\":" + std::to_string(state.audio_frames_decoded) +
    ",\"audio_pcm_samples_decoded\":" + std::to_string(state.audio_pcm_samples_decoded) +
    ",\"audio_pcm_samples_buffered\":" + std::to_string(state.audio_pcm_samples_buffered) +
    ",\"audio_outbound_frames_sent\":" + std::to_string(state.audio_outbound_frames_sent) +
    ",\"audio_pcm_samples_submitted_total\":" +
    std::to_string(state.audio_pcm_samples_submitted_total) +
    ",\"audio_last_outbound_samples\":" + std::to_string(state.audio_last_outbound_samples) +
    ",\"local_candidates_observed\":" + std::to_string(state.local_candidates_observed) +
    ",\"remote_candidates_seen\":" + std::to_string(state.remote_candidates_seen) +
    ",\"offers_seen\":" + std::to_string(state.offers_seen) +
    ",\"initial_remote_candidate_count\":" + std::to_string(initial_remote_candidate_count) +
    ",\"gather_started\":" + std::string(state.gather_started ? "true" : "false") +
    ",\"remote_description_applied\":" +
    std::string(state.remote_description_applied ? "true" : "false") +
    ",\"gathering_done\":" + std::string(state.gathering_done ? "true" : "false") +
    ",\"transport_connectivity_ready\":" +
    std::string(state.transport_connectivity_ready ? "true" : "false") +
    ",\"srtp_version\":\"" + json_escape(srtp_version ? std::string(srtp_version) : std::string("unknown")) + "\""
    ",\"usrsctp_initialized\":true";
  if (!state.dtls_fingerprint_sha256.empty()) {
    json += ",\"dtls_fingerprint_sha256\":\"" + json_escape(state.dtls_fingerprint_sha256) + "\"";
  }
  if (!state.dtls_certificate_subject.empty()) {
    json += ",\"dtls_certificate_subject\":\"" + json_escape(state.dtls_certificate_subject) + "\"";
  }
  if (!state.dtls_selected_srtp_profile.empty()) {
    json += ",\"dtls_selected_srtp_profile\":\"" + json_escape(state.dtls_selected_srtp_profile) + "\"";
  }
  if (!state.dtls_last_error.empty()) {
    json += ",\"dtls_last_error\":\"" + json_escape(state.dtls_last_error) + "\"";
  }
  if (!state.srtp_last_error.empty()) {
    json += ",\"srtp_last_error\":\"" + json_escape(state.srtp_last_error) + "\"";
  }
  if (state.rtp_last_payload_type >= 0) {
    json += ",\"rtp_last_payload_type\":" + std::to_string(state.rtp_last_payload_type);
  }
  if (state.rtp_last_sequence >= 0) {
    json += ",\"rtp_last_sequence\":" + std::to_string(state.rtp_last_sequence);
  }
  if (state.rtp_last_timestamp > 0) {
    json += ",\"rtp_last_timestamp\":" + std::to_string(state.rtp_last_timestamp);
  }
  if (state.rtp_last_ssrc > 0) {
    json += ",\"rtp_last_ssrc\":" + std::to_string(state.rtp_last_ssrc);
  }
  if (state.rtp_last_sent_payload_type >= 0) {
    json += ",\"rtp_last_sent_payload_type\":" +
            std::to_string(state.rtp_last_sent_payload_type);
  }
  if (state.rtp_last_sent_sequence >= 0) {
    json += ",\"rtp_last_sent_sequence\":" +
            std::to_string(state.rtp_last_sent_sequence);
  }
  if (state.rtp_last_sent_timestamp > 0 || state.rtp_packets_sent > 0) {
    json += ",\"rtp_last_sent_timestamp\":" +
            std::to_string(state.rtp_last_sent_timestamp);
  }
  if (state.rtp_last_sent_ssrc > 0) {
    json += ",\"rtp_last_sent_ssrc\":" + std::to_string(state.rtp_last_sent_ssrc);
  }
  if (state.rtcp_last_packet_type >= 0) {
    json += ",\"rtcp_last_packet_type\":" + std::to_string(state.rtcp_last_packet_type);
  }
  if (state.rtcp_last_ssrc > 0) {
    json += ",\"rtcp_last_ssrc\":" + std::to_string(state.rtcp_last_ssrc);
  }
  if (state.rtcp_last_sent_packet_type >= 0) {
    json += ",\"rtcp_last_sent_packet_type\":" +
            std::to_string(state.rtcp_last_sent_packet_type);
  }
  if (state.rtcp_last_sent_ssrc > 0) {
    json += ",\"rtcp_last_sent_ssrc\":" + std::to_string(state.rtcp_last_sent_ssrc);
  }
  if (state.audio_outbound_payload_type >= 0) {
    json += ",\"audio_outbound_payload_type\":" +
            std::to_string(state.audio_outbound_payload_type);
  }
  if (!state.audio_outbound_codec_name.empty()) {
    json += ",\"audio_outbound_codec_name\":\"" +
            json_escape(state.audio_outbound_codec_name) + "\"";
  }
  if (state.audio_outbound_sample_rate_hz > 0) {
    json += ",\"audio_outbound_sample_rate_hz\":" +
            std::to_string(state.audio_outbound_sample_rate_hz);
  }
  if (state.audio_outbound_channels > 0) {
    json += ",\"audio_outbound_channels\":" +
            std::to_string(state.audio_outbound_channels);
  }
  if (state.audio_last_sample_rate_hz > 0) {
    json += ",\"audio_last_sample_rate_hz\":" +
            std::to_string(state.audio_last_sample_rate_hz);
  }
  if (state.audio_last_channels > 0) {
    json += ",\"audio_last_channels\":" + std::to_string(state.audio_last_channels);
  }
  if (state.audio_last_frame_samples_per_channel > 0) {
    json += ",\"audio_last_frame_samples_per_channel\":" +
            std::to_string(state.audio_last_frame_samples_per_channel);
  }
  if (!state.audio_last_codec_name.empty()) {
    json += ",\"audio_last_codec_name\":\"" +
            json_escape(state.audio_last_codec_name) + "\"";
  }
  if (!state.audio_last_error.empty()) {
    json += ",\"audio_last_error\":\"" + json_escape(state.audio_last_error) + "\"";
  }
  if (!state.audio_outbound_last_error.empty()) {
    json += ",\"audio_outbound_last_error\":\"" +
            json_escape(state.audio_outbound_last_error) + "\"";
  }
  if (!state.rtcp_last_error.empty()) {
    json += ",\"rtcp_last_error\":\"" + json_escape(state.rtcp_last_error) + "\"";
  }
  if (!state.selected_local_candidate.empty()) {
    json += ",\"libjuice_selected_local_candidate\":\"" +
            json_escape(state.selected_local_candidate) + "\"";
  }
  if (!state.selected_remote_candidate.empty()) {
    json += ",\"libjuice_selected_remote_candidate\":\"" +
            json_escape(state.selected_remote_candidate) + "\"";
  }
  if (!state.selected_local_address.empty()) {
    json += ",\"libjuice_selected_local_address\":\"" +
            json_escape(state.selected_local_address) + "\"";
  }
  if (!state.selected_remote_address.empty()) {
    json += ",\"libjuice_selected_remote_address\":\"" +
            json_escape(state.selected_remote_address) + "\"";
  }
  if (!state.last_remote_description_error.empty()) {
    json += ",\"remote_description_error\":\"" + json_escape(state.last_remote_description_error) + "\"";
  }
  json += "}";
  return json;
}

int embedded_create(void** out_instance, char* err_buf, size_t err_buf_size) {
  if (!out_instance) {
    write_error("missing out_instance", err_buf, err_buf_size);
    return 0;
  }
  std::string err;
  if (!reserve_transport_runtime(&err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  *out_instance = new EmbeddedTransportState();
  return 1;
}

void embedded_destroy(void* instance) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (engine) {
    if (engine->agent) {
      juice_destroy(engine->agent);
      engine->agent = nullptr;
    }
    if (engine->inbound_srtp) {
      (void)srtp_dealloc(engine->inbound_srtp);
      engine->inbound_srtp = nullptr;
    }
    if (engine->outbound_srtp) {
      (void)srtp_dealloc(engine->outbound_srtp);
      engine->outbound_srtp = nullptr;
    }
    if (engine->dtls_ssl) {
      SSL_free(engine->dtls_ssl);
      engine->dtls_ssl = nullptr;
    }
    if (engine->dtls_ctx) {
      SSL_CTX_free(engine->dtls_ctx);
      engine->dtls_ctx = nullptr;
    }
    if (engine->dtls_certificate) {
      X509_free(engine->dtls_certificate);
      engine->dtls_certificate = nullptr;
    }
    if (engine->dtls_private_key) {
      EVP_PKEY_free(engine->dtls_private_key);
      engine->dtls_private_key = nullptr;
    }
#if defined(AGENTD_HAVE_OPUS)
    if (engine->outbound_opus_encoder) {
      opus_encoder_destroy(engine->outbound_opus_encoder);
      engine->outbound_opus_encoder = nullptr;
    }
#endif
    delete engine;
  }
  release_transport_runtime();
}

int embedded_initialize(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  refresh_transport_snapshot(engine);
  sync_async_progress_baseline(engine);
  const std::string payload = build_event_json(*engine, "media_engine_initialized", "signaling_ready", 0);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_handle_remote_description(
  void* instance,
  const char* description_type,
  const char* description_sdp,
  uint64_t initial_remote_candidate_count,
  char* answer_type_buf,
  size_t answer_type_buf_size,
  char* answer_sdp_buf,
  size_t answer_sdp_buf_size,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  const std::string type = description_type ? std::string(description_type) : std::string();
  if (!type.empty() && type != "offer") {
    write_error("expected remote offer", err_buf, err_buf_size);
    return 0;
  }
  engine->offers_seen += 1;
  engine->remote_description_applied = false;
  engine->last_remote_description_error.clear();

  const std::string remote_sdp = description_sdp ? std::string(description_sdp) : std::string();
  std::string audio_err;
  if (!remote_sdp.empty()) {
    if (!engine->audio_decoder.configure_from_remote_sdp(remote_sdp, &audio_err)) {
      engine->audio_last_error = audio_err;
      clear_outbound_audio_payload_selection(engine);
    } else {
      engine->audio_last_error.clear();
      (void)select_outbound_audio_payload_from_remote_sdp(engine);
    }
  } else {
    engine->audio_last_error = "remote SDP was empty";
    clear_outbound_audio_payload_selection(engine);
  }
  if (!remote_sdp.empty()) {
    const int rc = juice_set_remote_description(engine->agent, remote_sdp.c_str());
    if (rc == JUICE_ERR_SUCCESS) {
      engine->remote_description_applied = true;
      refresh_transport_snapshot(engine);
    } else {
      engine->last_remote_description_error =
        "libjuice remote description rejected with code " + std::to_string(rc);
    }
  } else {
    engine->last_remote_description_error = "remote SDP was empty";
  }
  if (initial_remote_candidate_count == 0) {
    (void)juice_set_remote_gathering_done(engine->agent);
  }
  wait_for_transport_progress(engine, 250);
  (void)advance_dtls_handshake(engine, nullptr);
  if (!refresh_local_description(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  refresh_transport_snapshot(engine);
  engine->last_answer_sdp_shape = contains_media_section(split_sdp_lines(remote_sdp))
    ? "browser_offer_mirrored_inactive"
    : "ice_only";
  sync_async_progress_baseline(engine);
  const std::string answer_sdp = build_answer_sdp(remote_sdp, *engine);

  if (!copy_text("answer", answer_type_buf, answer_type_buf_size) ||
      !copy_text(answer_sdp, answer_sdp_buf, answer_sdp_buf_size)) {
    write_error("answer buffer too small", err_buf, err_buf_size);
    return 0;
  }

  const std::string payload = build_event_json(*engine, "embedded_transport_answer_ready", "answer_ready",
                                               initial_remote_candidate_count);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_handle_remote_candidate(
  void* instance,
  const char* candidate,
  const char*,
  int,
  int,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  const std::string candidate_sdp = candidate ? std::string(candidate) : std::string();
  if (candidate_sdp.empty()) {
    write_error("remote candidate missing", err_buf, err_buf_size);
    return 0;
  }
  if (is_end_of_candidates_marker(trim_copy(candidate_sdp))) {
    if (juice_set_remote_gathering_done(engine->agent) != JUICE_ERR_SUCCESS) {
      write_error("libjuice remote gathering-done rejected", err_buf, err_buf_size);
      return 0;
    }
  } else {
    const int rc = juice_add_remote_candidate(engine->agent, candidate_sdp.c_str());
    if (rc != JUICE_ERR_SUCCESS) {
      write_error("libjuice remote candidate rejected with code " + std::to_string(rc), err_buf, err_buf_size);
      return 0;
    }
    engine->remote_candidates_seen += 1;
  }
  wait_for_transport_progress(engine, 1500);
  (void)advance_dtls_handshake(engine, nullptr);
  refresh_transport_snapshot(engine);
  sync_async_progress_baseline(engine);

  const std::string payload =
    build_event_json(*engine, "remote_candidate_ready", "signaling_active", 0);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

void embedded_handle_remote_bye(
  void* instance,
  const char* reason,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) return;
  refresh_transport_snapshot(engine);
  sync_async_progress_baseline(engine);
  std::string payload = build_event_json(*engine, "remote_bye", "stopped", 0);
  payload.pop_back();
  if (reason && reason[0]) {
    payload += ",\"reason\":\"" + json_escape(reason) + "\"";
  }
  payload += "}";
  (void)copy_text(payload, event_json_buf, event_json_buf_size);
}

void embedded_handle_local_shutdown(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) return;
  refresh_transport_snapshot(engine);
  sync_async_progress_baseline(engine);
  std::string payload = build_event_json(*engine, "local_bye_sent", "stopping", 0);
  payload.pop_back();
  payload += ",\"reason\":\"agentd_builtin_stop\"}";
  (void)copy_text(payload, event_json_buf, event_json_buf_size);
}

int embedded_poll_status(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  std::string payload;
  if (!pop_async_event_json(engine, &payload)) {
    if (event_json_buf && event_json_buf_size > 0) event_json_buf[0] = '\0';
    return 1;
  }
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_drain_audio(
  void* instance,
  int16_t* pcm_buf,
  size_t pcm_capacity_samples,
  size_t* out_pcm_samples,
  int* out_sample_rate_hz,
  int* out_channels,
  int* out_frame_samples_per_channel,
  char* codec_name_buf,
  size_t codec_name_buf_size,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  if (!pcm_buf || pcm_capacity_samples == 0) {
    write_error("missing pcm buffer", err_buf, err_buf_size);
    return 0;
  }
  if (out_pcm_samples) *out_pcm_samples = 0;
  if (out_sample_rate_hz) *out_sample_rate_hz = 0;
  if (out_channels) *out_channels = 0;
  if (out_frame_samples_per_channel) *out_frame_samples_per_channel = 0;
  if (codec_name_buf && codec_name_buf_size > 0) codec_name_buf[0] = '\0';
  if (event_json_buf && event_json_buf_size > 0) event_json_buf[0] = '\0';

  if (engine->pcm_staging.empty()) return 1;

  const size_t drain_samples = std::min(pcm_capacity_samples, engine->pcm_staging.size());
  for (size_t i = 0; i < drain_samples; ++i) {
    pcm_buf[i] = engine->pcm_staging.front();
    engine->pcm_staging.pop_front();
  }
  engine->audio_pcm_samples_buffered = engine->pcm_staging.size();

  if (out_pcm_samples) *out_pcm_samples = drain_samples;
  if (out_sample_rate_hz) {
    *out_sample_rate_hz = static_cast<int>(engine->audio_last_sample_rate_hz);
  }
  if (out_channels) *out_channels = static_cast<int>(engine->audio_last_channels);
  if (out_frame_samples_per_channel) {
    *out_frame_samples_per_channel =
      static_cast<int>(engine->audio_last_frame_samples_per_channel);
  }
  if (codec_name_buf && codec_name_buf_size > 0 && !engine->audio_last_codec_name.empty()) {
    if (!copy_text(engine->audio_last_codec_name, codec_name_buf, codec_name_buf_size)) {
      write_error("codec name buffer too small", err_buf, err_buf_size);
      return 0;
    }
  }

  refresh_transport_snapshot(engine);
  std::string payload = build_event_json(
    *engine,
    "audio_chunk_drained",
    derived_media_engine_state(*engine),
    0);
  payload.pop_back();
  payload += ",\"audio_last_drain_samples\":" + std::to_string(drain_samples) + "}";
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_submit_audio(
  void* instance,
  const int16_t* pcm_buf,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  int,
  const char*,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  if (event_json_buf && event_json_buf_size > 0) event_json_buf[0] = '\0';
  if (!pcm_buf || pcm_samples == 0) return 1;

  std::string err;
  if (!transmit_outbound_audio_rtp(
        engine, pcm_buf, pcm_samples, sample_rate_hz, channels, &err)) {
    engine->audio_outbound_last_error =
      err.empty() ? std::string("outbound RTP transmit failed") : err;
    std::string payload = build_event_json(
      *engine,
      "audio_chunk_transmit_failed",
      derived_media_engine_state(*engine),
      0);
    if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
      write_error("event buffer too small", err_buf, err_buf_size);
      return 0;
    }
    return 1;
  }

  refresh_transport_snapshot(engine);
  const std::string payload = build_event_json(
    *engine,
    "audio_chunk_transmitted",
    derived_media_engine_state(*engine),
    0);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

const agentd_voice_media_engine_provider_v5 kEmbeddedProvider = {
  AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V5,
  "builtin_native_plugin",
  1,
  kProviderName,
  kProviderVersion,
  kCapabilitiesJson,
  &embedded_create,
  &embedded_destroy,
  &embedded_initialize,
  &embedded_handle_remote_description,
  &embedded_handle_remote_candidate,
  &embedded_handle_remote_bye,
  &embedded_handle_local_shutdown,
  &embedded_poll_status,
  &embedded_drain_audio,
  &embedded_submit_audio,
};

}  // namespace

extern "C" const agentd_voice_media_engine_provider_v5* agentd_voice_media_engine_get_api_v5() {
  return &kEmbeddedProvider;
}
