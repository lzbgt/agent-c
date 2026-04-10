#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>
#include <openssl/x509.h>

#if defined(AGENTD_HAVE_DTLS_SRTP_SESSION_PAIR)
#include "session_voice_dtls_srtp_util.h"
#include <srtp2/srtp.h>
#endif

#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kDtlsSrtpProfiles =
  "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32";
constexpr const char* kDtlsSrtpExporterLabel = "EXTRACTOR-dtls_srtp";
constexpr size_t kDtlsSrtpExporterBytes = 2 * (16 + 14);
constexpr long kDtlsDatagramMtu = 1200;

std::string openssl_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

std::string ssl_error_text(SSL* ssl, int rc) {
  const int ssl_err = ssl ? SSL_get_error(ssl, rc) : SSL_ERROR_SSL;
  switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
      return "ssl_want_read";
    case SSL_ERROR_WANT_WRITE:
      return "ssl_want_write";
    case SSL_ERROR_ZERO_RETURN:
      return "ssl_zero_return";
    default:
      return "ssl_error_" + std::to_string(ssl_err) + ": " + openssl_error_text();
  }
}

std::string selected_srtp_profile_name(SSL* ssl) {
  if (!ssl) return "";
  SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(ssl);
  if (!profile || !profile->name) return "";
  return std::string(profile->name);
}

bool export_dtls_srtp_keying_material(SSL* ssl, unsigned char* out, size_t out_size) {
  if (!ssl || !out || out_size < kDtlsSrtpExporterBytes) return false;
  return SSL_export_keying_material(
           ssl,
           out,
           out_size,
           kDtlsSrtpExporterLabel,
           std::strlen(kDtlsSrtpExporterLabel),
           nullptr,
           0,
           0) == 1;
}

bool generate_ephemeral_dtls_identity(EVP_PKEY** out_key, X509** out_cert) {
  assert(out_key);
  assert(out_cert);
  *out_key = nullptr;
  *out_cert = nullptr;

  using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
  using ASN1IntPtr = std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)>;
  using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

  PkeyCtxPtr keygen_ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  if (!keygen_ctx) return false;
  if (EVP_PKEY_keygen_init(keygen_ctx.get()) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(keygen_ctx.get(), 2048) != 1) {
    return false;
  }

  EVP_PKEY* raw_key = nullptr;
  if (EVP_PKEY_keygen(keygen_ctx.get(), &raw_key) != 1 || !raw_key) return false;
  PkeyPtr key(raw_key, EVP_PKEY_free);

  X509Ptr cert(X509_new(), X509_free);
  if (!cert) return false;
  if (X509_set_version(cert.get(), 2) != 1) return false;

  unsigned char serial_bytes[8];
  if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) return false;
  BnPtr serial_bn(BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr), BN_free);
  if (!serial_bn) return false;
  ASN1IntPtr serial(BN_to_ASN1_INTEGER(serial_bn.get(), nullptr), ASN1_INTEGER_free);
  if (!serial || X509_set_serialNumber(cert.get(), serial.get()) != 1) return false;

  if (!X509_gmtime_adj(X509_get_notBefore(cert.get()), 0) ||
      !X509_gmtime_adj(X509_get_notAfter(cert.get()), 7 * 24 * 60 * 60L) ||
      X509_set_pubkey(cert.get(), key.get()) != 1) {
    return false;
  }

  X509_NAME* name = X509_get_subject_name(cert.get());
  if (!name ||
      X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("agentd builtin dtls loopback"),
        -1,
        -1,
        0) != 1 ||
      X509_set_issuer_name(cert.get(), name) != 1) {
    return false;
  }

  if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) return false;

  *out_key = key.release();
  *out_cert = cert.release();
  return true;
}

struct DtlsEndpoint {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  bool handshake_ready = false;
  std::string selected_srtp_profile;
  int packets_sent = 0;
  int packets_received = 0;
};

void destroy_endpoint(DtlsEndpoint* endpoint) {
  if (!endpoint) return;
  if (endpoint->ssl) {
    SSL_free(endpoint->ssl);
    endpoint->ssl = nullptr;
  }
  if (endpoint->ctx) {
    SSL_CTX_free(endpoint->ctx);
    endpoint->ctx = nullptr;
  }
}

bool configure_server_endpoint(DtlsEndpoint* endpoint, EVP_PKEY* key, X509* cert) {
  assert(endpoint);
  endpoint->ctx = SSL_CTX_new(DTLS_server_method());
  if (!endpoint->ctx) return false;
  if (SSL_CTX_set_min_proto_version(endpoint->ctx, DTLS1_2_VERSION) != 1) return false;
  SSL_CTX_set_read_ahead(endpoint->ctx, 1);
  if (SSL_CTX_use_certificate(endpoint->ctx, cert) != 1 ||
      SSL_CTX_use_PrivateKey(endpoint->ctx, key) != 1 ||
      SSL_CTX_check_private_key(endpoint->ctx) != 1) {
    return false;
  }
  if (SSL_CTX_set_tlsext_use_srtp(endpoint->ctx, kDtlsSrtpProfiles) != 0) return false;

  endpoint->ssl = SSL_new(endpoint->ctx);
  if (!endpoint->ssl) return false;
  BIO* rbio = BIO_new(BIO_s_dgram_mem());
  BIO* wbio = BIO_new(BIO_s_dgram_mem());
  assert(rbio);
  assert(wbio);
  (void)BIO_ctrl(rbio, BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  (void)BIO_ctrl(wbio, BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  SSL_set_bio(endpoint->ssl, rbio, wbio);
  SSL_set_options(endpoint->ssl, SSL_OP_NO_QUERY_MTU);
  SSL_set_mtu(endpoint->ssl, kDtlsDatagramMtu);
  SSL_set_accept_state(endpoint->ssl);
  return true;
}

bool configure_client_endpoint(DtlsEndpoint* endpoint) {
  assert(endpoint);
  endpoint->ctx = SSL_CTX_new(DTLS_client_method());
  if (!endpoint->ctx) return false;
  if (SSL_CTX_set_min_proto_version(endpoint->ctx, DTLS1_2_VERSION) != 1) return false;
  SSL_CTX_set_read_ahead(endpoint->ctx, 1);
  SSL_CTX_set_verify(endpoint->ctx, SSL_VERIFY_NONE, nullptr);
  if (SSL_CTX_set_tlsext_use_srtp(endpoint->ctx, kDtlsSrtpProfiles) != 0) return false;

  endpoint->ssl = SSL_new(endpoint->ctx);
  if (!endpoint->ssl) return false;
  BIO* rbio = BIO_new(BIO_s_dgram_mem());
  BIO* wbio = BIO_new(BIO_s_dgram_mem());
  assert(rbio);
  assert(wbio);
  (void)BIO_ctrl(rbio, BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  (void)BIO_ctrl(wbio, BIO_CTRL_DGRAM_SET_MTU, kDtlsDatagramMtu, nullptr);
  SSL_set_bio(endpoint->ssl, rbio, wbio);
  SSL_set_options(endpoint->ssl, SSL_OP_NO_QUERY_MTU);
  SSL_set_mtu(endpoint->ssl, kDtlsDatagramMtu);
  SSL_set_connect_state(endpoint->ssl);
  return true;
}

void pump_outbound_packets(DtlsEndpoint* from, DtlsEndpoint* to) {
  assert(from);
  assert(to);
  BIO* wbio = SSL_get_wbio(from->ssl);
  BIO* rbio = SSL_get_rbio(to->ssl);
  assert(wbio);
  assert(rbio);
  char packet[2048];
  for (;;) {
    const int n = BIO_read(wbio, packet, sizeof(packet));
    if (n <= 0) break;
    const int wrote = BIO_write(rbio, packet, n);
    assert(wrote == n);
    from->packets_sent += 1;
    to->packets_received += 1;
  }
}

void advance_handshake(DtlsEndpoint* endpoint) {
  assert(endpoint);
  if (endpoint->handshake_ready) return;
  const int rc = SSL_do_handshake(endpoint->ssl);
  if (rc == 1) {
    endpoint->handshake_ready = true;
    endpoint->selected_srtp_profile = selected_srtp_profile_name(endpoint->ssl);
    return;
  }
  const int ssl_err = SSL_get_error(endpoint->ssl, rc);
  assert(
    ssl_err == SSL_ERROR_WANT_READ ||
    ssl_err == SSL_ERROR_WANT_WRITE
  );
}

void test_dtls_memory_loopback_negotiates_srtp_and_exporter() {
  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  assert(generate_ephemeral_dtls_identity(&key, &cert));

  DtlsEndpoint server;
  DtlsEndpoint client;
  assert(configure_server_endpoint(&server, key, cert));
  assert(configure_client_endpoint(&client));

  for (int i = 0; i < 128; ++i) {
    advance_handshake(&client);
    pump_outbound_packets(&client, &server);
    advance_handshake(&server);
    pump_outbound_packets(&server, &client);
    if (client.handshake_ready && server.handshake_ready) break;
  }

  assert(client.handshake_ready);
  assert(server.handshake_ready);
  assert(client.selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");
  assert(server.selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");
  assert(client.packets_sent > 0);
  assert(client.packets_received > 0);
  assert(server.packets_sent > 0);
  assert(server.packets_received > 0);

  unsigned char client_keymat[kDtlsSrtpExporterBytes];
  unsigned char server_keymat[kDtlsSrtpExporterBytes];
  assert(export_dtls_srtp_keying_material(client.ssl, client_keymat, sizeof(client_keymat)));
  assert(export_dtls_srtp_keying_material(server.ssl, server_keymat, sizeof(server_keymat)));
  assert(std::memcmp(client_keymat, server_keymat, sizeof(client_keymat)) == 0);
  OPENSSL_cleanse(client_keymat, sizeof(client_keymat));
  OPENSSL_cleanse(server_keymat, sizeof(server_keymat));

  destroy_endpoint(&client);
  destroy_endpoint(&server);
  EVP_PKEY_free(key);
  X509_free(cert);
}

#if defined(AGENTD_HAVE_DTLS_SRTP_SESSION_PAIR)
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

void test_dtls_memory_loopback_derives_srtp_contexts_and_round_trips_rtp() {
  assert(srtp_init() == srtp_err_status_ok);

  EVP_PKEY* key = nullptr;
  X509* cert = nullptr;
  assert(generate_ephemeral_dtls_identity(&key, &cert));

  DtlsEndpoint server;
  DtlsEndpoint client;
  assert(configure_server_endpoint(&server, key, cert));
  assert(configure_client_endpoint(&client));

  for (int i = 0; i < 128; ++i) {
    advance_handshake(&client);
    pump_outbound_packets(&client, &server);
    advance_handshake(&server);
    pump_outbound_packets(&server, &client);
    if (client.handshake_ready && server.handshake_ready) break;
  }

  assert(client.handshake_ready);
  assert(server.handshake_ready);
  assert(server.selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");
  assert(client.selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");

  agentd::DtlsSrtpProfileSpec profile;
  std::string err;
  assert(agentd::resolve_dtls_srtp_profile_spec(server.selected_srtp_profile, &profile, &err));
  assert(err.empty());

  std::vector<unsigned char> exporter_keying_material;
  assert(agentd::export_dtls_srtp_keying_material(server.ssl, profile, &exporter_keying_material, &err));
  assert(err.empty());

  agentd::DtlsSrtpKeyBlock key_block;
  assert(agentd::derive_dtls_srtp_key_block(
    profile,
    exporter_keying_material.data(),
    exporter_keying_material.size(),
    &key_block,
    &err));
  assert(err.empty());

  agentd::DtlsSrtpSessionPair server_sessions;
  agentd::DtlsSrtpSessionPair client_sessions;
  assert(agentd::create_dtls_srtp_session_pair(
    key_block, agentd::DtlsSrtpLocalRole::server, &server_sessions, &err));
  assert(err.empty());
  assert(agentd::create_dtls_srtp_session_pair(
    key_block, agentd::DtlsSrtpLocalRole::client, &client_sessions, &err));
  assert(err.empty());
  assert(server_sessions.inbound_ready);
  assert(server_sessions.outbound_ready);
  assert(client_sessions.inbound_ready);
  assert(client_sessions.outbound_ready);

  std::array<unsigned char, 256> packet{};
  const char payload[] = "embedded-srtp-rtp";
  const int plain_len = 12 + static_cast<int>(sizeof(payload) - 1);
  packet[0] = 0x80;
  packet[1] = 111;
  write_u16_be(packet.data() + 2, 1);
  write_u32_be(packet.data() + 4, 0x01020304u);
  write_u32_be(packet.data() + 8, 0x11223344u);
  std::memcpy(packet.data() + 12, payload, sizeof(payload) - 1);

  int protected_len = plain_len;
  assert(srtp_protect(server_sessions.outbound, packet.data(), &protected_len) == srtp_err_status_ok);
  assert(protected_len > plain_len);

  int unprotected_len = protected_len;
  assert(srtp_unprotect(client_sessions.inbound, packet.data(), &unprotected_len) == srtp_err_status_ok);
  assert(unprotected_len == plain_len);
  assert(std::memcmp(packet.data() + 12, payload, sizeof(payload) - 1) == 0);

  agentd::destroy_dtls_srtp_session_pair(&client_sessions);
  agentd::destroy_dtls_srtp_session_pair(&server_sessions);
  destroy_endpoint(&client);
  destroy_endpoint(&server);
  EVP_PKEY_free(key);
  X509_free(cert);
  assert(srtp_shutdown() == srtp_err_status_ok);
}
#endif

}  // namespace

int main() {
  test_dtls_memory_loopback_negotiates_srtp_and_exporter();
#if defined(AGENTD_HAVE_DTLS_SRTP_SESSION_PAIR)
  test_dtls_memory_loopback_derives_srtp_contexts_and_round_trips_rtp();
#endif
  return 0;
}
