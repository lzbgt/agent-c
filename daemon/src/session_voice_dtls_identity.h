#pragma once

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <string>

namespace agentd {

std::string openssl_last_error_text();

bool generate_ephemeral_dtls_identity(
  EVP_PKEY** out_private_key,
  X509** out_certificate,
  std::string* out_fingerprint_sha256,
  std::string* out_certificate_subject,
  std::string* out_err);

}  // namespace agentd
