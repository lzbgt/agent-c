#include "session_voice_builtin_media_engine_plugin.h"

#include <juice/juice.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <srtp2/srtp.h>
#include <usrsctp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kProviderName = "agentd_builtin_embedded_transport_provider";
constexpr const char* kProviderVersion = "0.3.0";
constexpr const char* kCapabilitiesJson =
  "{\"signaling\":true,\"audio_capture\":false,\"audio_render\":false,"
  "\"ice\":true,\"dtls\":false,\"dtls_identity\":true,\"dtls_answer_shape\":true,"
  "\"srtp\":true,\"sctp\":true,"
  "\"transport_family\":\"embedded_transport_primitives\","
  "\"embedded_transport_provider\":true,\"sample_provider\":false,"
  "\"real_media_engine\":false,\"remote_description_optional\":true,"
  "\"candidate_trickle_ingest\":true,\"candidate_trickle_emit\":false}";

struct EmbeddedTransportState {
  juice_agent_t* agent = nullptr;
  std::string local_description;
  std::string libjuice_state = "disconnected";
  std::string last_remote_description_error;
  EVP_PKEY* dtls_private_key = nullptr;
  X509* dtls_certificate = nullptr;
  std::string dtls_fingerprint_sha256;
  std::string dtls_setup_role = "passive";
  std::string dtls_certificate_subject;
  std::string selected_local_candidate;
  std::string selected_remote_candidate;
  std::string selected_local_address;
  std::string selected_remote_address;
  std::string last_answer_sdp_shape = "ice_only";
  uint64_t offers_seen = 0;
  uint64_t remote_candidates_seen = 0;
  uint64_t local_candidates_observed = 0;
  bool gathering_done = false;
  bool gather_started = false;
  bool remote_description_applied = false;
  bool transport_connectivity_ready = false;
  bool dtls_identity_ready = false;
};

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
}

bool ensure_agent(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->agent) return true;
  if (!generate_dtls_identity(engine, out_err)) return false;

  juice_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  cfg.cb_state_changed = &on_state_changed;
  cfg.cb_candidate = &on_candidate;
  cfg.cb_gathering_done = &on_gathering_done;
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
    ",\"native_media_supported\":false,\"native_media_active\":false"
    ",\"provider\":\"" + std::string(kProviderName) + "\""
    ",\"transport_family\":\"embedded_transport_primitives\""
    ",\"dtls_identity_ready\":" + std::string(state.dtls_identity_ready ? "true" : "false") +
    ",\"dtls_setup_role\":\"" + json_escape(state.dtls_setup_role) + "\""
    ",\"sdp_answer_shape\":\"" + json_escape(state.last_answer_sdp_shape) + "\""
    ",\"libjuice_state\":\"" + json_escape(state.libjuice_state) + "\""
    ",\"libjuice_local_description_bytes\":" + std::to_string(state.local_description.size()) +
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
    if (engine->dtls_certificate) {
      X509_free(engine->dtls_certificate);
      engine->dtls_certificate = nullptr;
    }
    if (engine->dtls_private_key) {
      EVP_PKEY_free(engine->dtls_private_key);
      engine->dtls_private_key = nullptr;
    }
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
  if (!refresh_local_description(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  refresh_transport_snapshot(engine);
  engine->last_answer_sdp_shape = contains_media_section(split_sdp_lines(remote_sdp))
    ? "browser_offer_mirrored_inactive"
    : "ice_only";
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
  const int rc = juice_add_remote_candidate(engine->agent, candidate_sdp.c_str());
  if (rc != JUICE_ERR_SUCCESS) {
    write_error("libjuice remote candidate rejected with code " + std::to_string(rc), err_buf, err_buf_size);
    return 0;
  }
  engine->remote_candidates_seen += 1;
  wait_for_transport_progress(engine, 1500);
  refresh_transport_snapshot(engine);

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
  std::string payload = build_event_json(*engine, "local_bye_sent", "stopping", 0);
  payload.pop_back();
  payload += ",\"reason\":\"agentd_builtin_stop\"}";
  (void)copy_text(payload, event_json_buf, event_json_buf_size);
}

const agentd_voice_media_engine_provider_v2 kEmbeddedProvider = {
  AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V2,
  "builtin_native_plugin",
  0,
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
};

}  // namespace

extern "C" const agentd_voice_media_engine_provider_v2* agentd_voice_media_engine_get_api_v2() {
  return &kEmbeddedProvider;
}
