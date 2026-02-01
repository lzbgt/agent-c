#include "base64.h"

#include <array>
#include <cctype>
#include <cstdint>

static inline bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string base64_encode(const void* data, size_t len) {
  static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  if (!bytes || len == 0) return "";

  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t n = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8) | (uint32_t)bytes[i + 2];
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(kAlphabet[(n >> 6) & 0x3f]);
    out.push_back(kAlphabet[n & 0x3f]);
    i += 3;
  }

  const size_t rem = len - i;
  if (rem == 1) {
    const uint32_t n = ((uint32_t)bytes[i] << 16);
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t n = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8);
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(kAlphabet[(n >> 6) & 0x3f]);
    out.push_back('=');
  }

  return out;
}

static std::array<int8_t, 256> make_decode_table() {
  std::array<int8_t, 256> t{};
  t.fill(-1);
  const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; i++) {
    t[(unsigned char)a[i]] = (int8_t)i;
  }
  t[(unsigned char)'='] = -2;  // padding marker
  return t;
}

bool base64_decode(const std::string& b64, std::string* out_bytes, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_bytes) out_bytes->clear();
  if (!out_bytes) {
    if (out_error) *out_error = "missing out_bytes";
    return false;
  }

  static const std::array<int8_t, 256> kDec = make_decode_table();

  // Filter/validate into 4-char blocks.
  std::string cleaned;
  cleaned.reserve(b64.size());
  for (char c : b64) {
    if (is_ascii_space(c)) continue;
    cleaned.push_back(c);
  }

  if (cleaned.empty()) return true;
  if ((cleaned.size() % 4) != 0) {
    if (out_error) *out_error = "invalid base64 length (must be multiple of 4)";
    return false;
  }

  out_bytes->reserve((cleaned.size() / 4) * 3);

  for (size_t i = 0; i < cleaned.size(); i += 4) {
    const unsigned char c0 = (unsigned char)cleaned[i + 0];
    const unsigned char c1 = (unsigned char)cleaned[i + 1];
    const unsigned char c2 = (unsigned char)cleaned[i + 2];
    const unsigned char c3 = (unsigned char)cleaned[i + 3];

    const int8_t v0 = kDec[c0];
    const int8_t v1 = kDec[c1];
    const int8_t v2 = kDec[c2];
    const int8_t v3 = kDec[c3];

    if (v0 < 0 || v1 < 0) {
      if (out_error) *out_error = "invalid base64 character";
      return false;
    }
    if (v2 == -2 && v3 != -2) {
      if (out_error) *out_error = "invalid base64 padding";
      return false;
    }
    if ((v2 < 0 && v2 != -2) || (v3 < 0 && v3 != -2)) {
      if (out_error) *out_error = "invalid base64 character";
      return false;
    }

    const uint32_t n = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                       ((uint32_t)((v2 == -2) ? 0 : v2) << 6) |
                       (uint32_t)((v3 == -2) ? 0 : v3);

    out_bytes->push_back((char)((n >> 16) & 0xff));
    if (v2 != -2) out_bytes->push_back((char)((n >> 8) & 0xff));
    if (v3 != -2) out_bytes->push_back((char)(n & 0xff));
  }

  return true;
}

