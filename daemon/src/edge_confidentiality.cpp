#include "edge_confidentiality.h"

#include "json_util.h"
#include "string_util.h"

#include "base64.h"

#include <json/json.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <vector>

namespace agentd {
namespace {

static std::string openssl_last_error_text() {
  const unsigned long err = ERR_get_error();
  if (err == 0) return "openssl_error";
  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf[0] ? std::string(buf) : std::string("openssl_error");
}

static bool derive_aes256_key(const std::string& secret, std::array<uint8_t, 32>* out_key, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out_key) return false;
  out_key->fill(0);
  if (secret.empty()) {
    if (out_error) *out_error = "empty secret";
    return false;
  }
  unsigned int md_len = 0;
  if (EVP_Digest(
        secret.data(),
        secret.size(),
        out_key->data(),
        &md_len,
        EVP_sha256(),
        nullptr) != 1 || md_len != out_key->size()) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  return true;
}

static bool aes_256_gcm_encrypt(
  const std::array<uint8_t, 32>& key,
  const std::string& plaintext,
  std::string* out_iv,
  std::string* out_ct,
  std::string* out_tag,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_iv || !out_ct || !out_tag) return false;
  out_iv->clear();
  out_ct->clear();
  out_tag->clear();

  std::array<uint8_t, 12> iv{};
  std::array<uint8_t, 16> tag{};
  if (RAND_bytes(iv.data(), (int)iv.size()) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }

  std::vector<uint8_t> ct(plaintext.size() + 16u, 0);
  int outl = 0;
  int total = 0;
  EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
  if (!raw) {
    if (out_error) *out_error = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(raw, EVP_CIPHER_CTX_free);
  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, (int)iv.size(), nullptr) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(
          ctx.get(),
          ct.data(),
          &outl,
          reinterpret_cast<const uint8_t*>(plaintext.data()),
          (int)plaintext.size()) != 1) {
      if (out_error) *out_error = openssl_last_error_text();
      return false;
    }
    total += outl;
  }
  if (EVP_EncryptFinal_ex(ctx.get(), ct.data() + total, &outl) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  total += outl;
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, (int)tag.size(), tag.data()) != 1) {
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }

  *out_iv = base64_encode(reinterpret_cast<const char*>(iv.data()), iv.size());
  *out_ct = base64_encode(reinterpret_cast<const char*>(ct.data()), (size_t)total);
  *out_tag = base64_encode(reinterpret_cast<const char*>(tag.data()), tag.size());
  return true;
}

static bool aes_256_gcm_decrypt(
  const std::array<uint8_t, 32>& key,
  const std::string& iv,
  const std::string& ct,
  const std::string& tag,
  std::string* out_plaintext,
  std::string* out_error_code,
  std::string* out_error
) {
  if (out_error_code) out_error_code->clear();
  if (out_error) out_error->clear();
  if (!out_plaintext) return false;
  out_plaintext->clear();

  std::string iv_bytes;
  std::string ct_bytes;
  std::string tag_bytes;
  std::string berr;
  if (!base64_decode(iv, &iv_bytes, &berr) || iv_bytes.size() != 12) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "body_enc.iv must be base64 of 12 bytes";
    return false;
  }
  if (!base64_decode(tag, &tag_bytes, &berr) || tag_bytes.size() != 16) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "body_enc.tag must be base64 of 16 bytes";
    return false;
  }
  if (!base64_decode(ct, &ct_bytes, &berr)) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "body_enc.ct must be valid base64";
    return false;
  }

  std::string pt;
  pt.resize(ct_bytes.size());
  int outl = 0;
  int total = 0;
  EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
  if (!raw) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(raw, EVP_CIPHER_CTX_free);
  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, (int)iv_bytes.size(), nullptr) != 1) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (EVP_DecryptInit_ex(
        ctx.get(),
        nullptr,
        nullptr,
        key.data(),
        reinterpret_cast<const uint8_t*>(iv_bytes.data())) != 1) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  if (!ct_bytes.empty()) {
    if (EVP_DecryptUpdate(
          ctx.get(),
          reinterpret_cast<uint8_t*>(&pt[0]),
          &outl,
          reinterpret_cast<const uint8_t*>(ct_bytes.data()),
          (int)ct_bytes.size()) != 1) {
      if (out_error_code) *out_error_code = "decrypt_failed";
      if (out_error) *out_error = "decrypt failed";
      return false;
    }
    total += outl;
  }
  if (EVP_CIPHER_CTX_ctrl(
        ctx.get(),
        EVP_CTRL_GCM_SET_TAG,
        (int)tag_bytes.size(),
        reinterpret_cast<void*>(&tag_bytes[0])) != 1) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = openssl_last_error_text();
    return false;
  }
  const int final_ok = EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<uint8_t*>(&pt[0]) + total, &outl);
  if (final_ok != 1) {
    if (out_error_code) *out_error_code = "decrypt_failed";
    if (out_error) *out_error = "decrypt failed";
    return false;
  }
  total += outl;
  pt.resize((size_t)total);
  *out_plaintext = std::move(pt);
  return true;
}

static bool validate_confidential_kid(const std::string& kid) {
  if (kid.empty() || kid.size() > 64) return false;
  for (const char c : kid) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool edge_confidentiality_seal_json_object(
  const std::string& kid,
  const std::string& secret,
  const Json::Value& body,
  Json::Value* out_body_enc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_body_enc) return false;
  *out_body_enc = Json::Value(Json::nullValue);
  if (!validate_confidential_kid(kid)) {
    if (out_error) *out_error = "invalid kid";
    return false;
  }
  if (!body.isObject()) {
    if (out_error) *out_error = "body must be a JSON object";
    return false;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string plaintext = Json::writeString(wb, body);

  std::array<uint8_t, 32> key{};
  std::string derr;
  if (!derive_aes256_key(secret, &key, &derr)) {
    if (out_error) *out_error = derr.empty() ? "failed to derive key" : derr;
    return false;
  }

  std::string iv_b64;
  std::string ct_b64;
  std::string tag_b64;
  if (!aes_256_gcm_encrypt(key, plaintext, &iv_b64, &ct_b64, &tag_b64, &derr)) {
    if (out_error) *out_error = derr.empty() ? "encrypt failed" : derr;
    return false;
  }

  Json::Value enc(Json::objectValue);
  enc["schema"] = "umbmp_body_enc_v1";
  enc["alg"] = "aes-256-gcm";
  enc["kid"] = kid;
  enc["iv"] = iv_b64;
  enc["tag"] = tag_b64;
  enc["ct"] = ct_b64;
  enc["content_type"] = "application/json";
  *out_body_enc = enc;
  return true;
}

bool edge_confidentiality_open_json_object(
  const Json::Value& body_enc,
  const std::map<std::string, std::string>& keys,
  Json::Value* out_body,
  std::string* out_kid,
  std::string* out_error_code,
  std::string* out_error
) {
  if (out_error_code) out_error_code->clear();
  if (out_error) out_error->clear();
  if (out_body) *out_body = Json::Value(Json::nullValue);
  if (out_kid) out_kid->clear();

  if (!body_enc.isObject()) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "body_enc must be an object";
    return false;
  }
  const std::string schema =
    body_enc.isMember("schema") && body_enc["schema"].isString() ? trim_copy(body_enc["schema"].asString()) : "";
  const std::string alg =
    body_enc.isMember("alg") && body_enc["alg"].isString() ? trim_copy(body_enc["alg"].asString()) : "";
  const std::string kid =
    body_enc.isMember("kid") && body_enc["kid"].isString() ? trim_copy(body_enc["kid"].asString()) : "";
  const std::string iv =
    body_enc.isMember("iv") && body_enc["iv"].isString() ? trim_copy(body_enc["iv"].asString()) : "";
  const std::string tag =
    body_enc.isMember("tag") && body_enc["tag"].isString() ? trim_copy(body_enc["tag"].asString()) : "";
  const std::string ct =
    body_enc.isMember("ct") && body_enc["ct"].isString() ? trim_copy(body_enc["ct"].asString()) : "";
  const std::string content_type =
    body_enc.isMember("content_type") && body_enc["content_type"].isString() ? trim_copy(body_enc["content_type"].asString()) : "";
  if (schema != "umbmp_body_enc_v1") {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "unsupported body_enc.schema";
    return false;
  }
  if (alg != "aes-256-gcm") {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "unsupported body_enc.alg";
    return false;
  }
  if (!validate_confidential_kid(kid)) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "invalid body_enc.kid";
    return false;
  }
  if (content_type != "application/json") {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "unsupported body_enc.content_type";
    return false;
  }

  const auto it = keys.find(kid);
  if (it == keys.end() || trim_copy(it->second).empty()) {
    if (out_error_code) *out_error_code = "unknown_confidential_kid";
    if (out_error) *out_error = "unknown body_enc.kid";
    return false;
  }

  std::array<uint8_t, 32> key{};
  std::string derr;
  if (!derive_aes256_key(it->second, &key, &derr)) {
    if (out_error_code) *out_error_code = "internal";
    if (out_error) *out_error = derr.empty() ? "failed to derive key" : derr;
    return false;
  }

  std::string plaintext;
  std::string decrypt_code;
  if (!aes_256_gcm_decrypt(key, iv, ct, tag, &plaintext, &decrypt_code, &derr)) {
    if (out_error_code) *out_error_code = decrypt_code.empty() ? "decrypt_failed" : decrypt_code;
    if (out_error) *out_error = derr.empty() ? "decrypt failed" : derr;
    return false;
  }

  Json::Value body;
  if (!json_parse_any(plaintext, &body, &derr) || !body.isObject()) {
    if (out_error_code) *out_error_code = "invalid_body_enc";
    if (out_error) *out_error = "decrypted body must be a JSON object";
    return false;
  }

  if (out_kid) *out_kid = kid;
  if (out_body) *out_body = body;
  return true;
}

bool edge_confidentiality_wrap_envelope_body(
  Json::Value* envelope_io,
  const std::map<std::string, std::string>& keys,
  const std::string& kid,
  std::string* out_error_code,
  std::string* out_error
) {
  if (out_error_code) out_error_code->clear();
  if (out_error) out_error->clear();
  if (!envelope_io || !envelope_io->isObject()) {
    if (out_error_code) *out_error_code = "invalid_envelope";
    if (out_error) *out_error = "envelope must be an object";
    return false;
  }
  if (!envelope_io->isMember("body") || !(*envelope_io)["body"].isObject()) {
    if (out_error_code) *out_error_code = "invalid_body";
    if (out_error) *out_error = "envelope.body must be a JSON object";
    return false;
  }
  const auto it = keys.find(kid);
  if (it == keys.end() || trim_copy(it->second).empty()) {
    if (out_error_code) *out_error_code = "unknown_confidential_kid";
    if (out_error) *out_error = "unknown confidentiality kid";
    return false;
  }

  Json::Value enc;
  std::string serr;
  if (!edge_confidentiality_seal_json_object(kid, it->second, (*envelope_io)["body"], &enc, &serr)) {
    if (out_error_code) *out_error_code = "encrypt_failed";
    if (out_error) *out_error = serr.empty() ? "encrypt failed" : serr;
    return false;
  }
  envelope_io->removeMember("body");
  (*envelope_io)["body_enc"] = enc;
  return true;
}

bool edge_confidentiality_extract_envelope_body(
  const Json::Value& envelope,
  const std::map<std::string, std::string>& keys,
  bool required,
  Json::Value* out_body,
  bool* out_used_encryption,
  std::string* out_kid,
  std::string* out_error_code,
  std::string* out_error
) {
  if (out_error_code) out_error_code->clear();
  if (out_error) out_error->clear();
  if (out_body) *out_body = Json::Value(Json::nullValue);
  if (out_used_encryption) *out_used_encryption = false;
  if (out_kid) out_kid->clear();

  const bool has_body = envelope.isMember("body") && !envelope["body"].isNull();
  const bool has_body_enc = envelope.isMember("body_enc") && !envelope["body_enc"].isNull();
  if (has_body && has_body_enc) {
    if (out_error_code) *out_error_code = "invalid_envelope";
    if (out_error) *out_error = "envelope cannot contain both body and body_enc";
    return false;
  }
  if (has_body_enc) {
    if (out_used_encryption) *out_used_encryption = true;
    return edge_confidentiality_open_json_object(
      envelope["body_enc"], keys, out_body, out_kid, out_error_code, out_error);
  }
  if (required) {
    if (out_error_code) *out_error_code = "confidentiality_required";
    if (out_error) *out_error = "edge confidentiality required: missing body_enc";
    return false;
  }
  if (!has_body || !envelope["body"].isObject()) {
    if (out_error_code) *out_error_code = "invalid_body";
    if (out_error) *out_error = "invalid envelope (missing body/body_enc)";
    return false;
  }
  if (out_body) *out_body = envelope["body"];
  return true;
}

}  // namespace agentd
