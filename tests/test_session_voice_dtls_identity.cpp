#include "session_voice_dtls_identity.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <cassert>
#include <string>

namespace {

void test_generate_ephemeral_dtls_identity_returns_usable_keypair() {
  EVP_PKEY* private_key = nullptr;
  X509* certificate = nullptr;
  std::string fingerprint;
  std::string subject;
  std::string err;

  assert(agentd::generate_ephemeral_dtls_identity(
    &private_key,
    &certificate,
    &fingerprint,
    &subject,
    &err));
  assert(err.empty());
  assert(private_key);
  assert(certificate);
  assert(!fingerprint.empty());
  assert(fingerprint.size() == 95);
  assert(fingerprint.find(':') != std::string::npos);
  assert(subject.find("agentd builtin embedded transport") != std::string::npos);
  assert(X509_check_private_key(certificate, private_key) == 1);

  X509_free(certificate);
  EVP_PKEY_free(private_key);
}

void test_generate_ephemeral_dtls_identity_rejects_missing_outputs() {
  X509* certificate = nullptr;
  std::string fingerprint;
  std::string subject;
  std::string err;
  assert(!agentd::generate_ephemeral_dtls_identity(
    nullptr,
    &certificate,
    &fingerprint,
    &subject,
    &err));
  assert(!err.empty());
  assert(certificate == nullptr);
  assert(fingerprint.empty());
  assert(subject.empty());
}

}  // namespace

int main() {
  test_generate_ephemeral_dtls_identity_returns_usable_keypair();
  test_generate_ephemeral_dtls_identity_rejects_missing_outputs();
  return 0;
}
