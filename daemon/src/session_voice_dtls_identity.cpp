#include "session_voice_dtls_identity.h"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <cstring>
#include <memory>

namespace agentd {
namespace {

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

}  // namespace

std::string openssl_last_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256];
  std::memset(buf, 0, sizeof(buf));
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

bool generate_ephemeral_dtls_identity(
  EVP_PKEY** out_private_key,
  X509** out_certificate,
  std::string* out_fingerprint_sha256,
  std::string* out_certificate_subject,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_private_key) *out_private_key = nullptr;
  if (out_certificate) *out_certificate = nullptr;
  if (out_fingerprint_sha256) out_fingerprint_sha256->clear();
  if (out_certificate_subject) out_certificate_subject->clear();
  if (!out_private_key || !out_certificate ||
      !out_fingerprint_sha256 || !out_certificate_subject) {
    if (out_err) *out_err = "missing DTLS identity output";
    return false;
  }

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

  const std::string fingerprint = sha256_fingerprint_text(cert.get());
  if (fingerprint.empty()) {
    if (out_err) *out_err = "openssl certificate fingerprint generation failed";
    return false;
  }

  *out_fingerprint_sha256 = fingerprint;
  *out_certificate_subject = x509_name_to_string(X509_get_subject_name(cert.get()));
  *out_private_key = key.release();
  *out_certificate = cert.release();
  return true;
}

}  // namespace agentd
