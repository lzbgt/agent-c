#include "agent/base64.h"

#include <string.h>

static const char kB64Alphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t agent_base64_encode_len(size_t n) {
  // 4 chars per 3 bytes, rounded up.
  return ((n + 2) / 3) * 4;
}

static agent_status_t ensure_chars(char* out, size_t out_cap, size_t need_chars_excluding_nul) {
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (need_chars_excluding_nul > (out_cap - 1)) return AGENT_ERR_BOUNDS;
  return AGENT_OK;
}

agent_status_t agent_base64_encode(
  const uint8_t* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';
  if (!in && in_len != 0) return AGENT_ERR_INVALID_ARGUMENT;

  const size_t need = agent_base64_encode_len(in_len);
  agent_status_t st = ensure_chars(out, out_cap, need);
  if (st != AGENT_OK) return st;

  size_t o = 0;
  size_t i = 0;
  while (i + 3 <= in_len) {
    const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
    out[o++] = kB64Alphabet[(v >> 18) & 0x3f];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3f];
    out[o++] = kB64Alphabet[(v >> 6) & 0x3f];
    out[o++] = kB64Alphabet[v & 0x3f];
    i += 3;
  }

  const size_t rem = in_len - i;
  if (rem == 1) {
    const uint32_t v = ((uint32_t)in[i] << 16);
    out[o++] = kB64Alphabet[(v >> 18) & 0x3f];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3f];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = kB64Alphabet[(v >> 18) & 0x3f];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3f];
    out[o++] = kB64Alphabet[(v >> 6) & 0x3f];
    out[o++] = '=';
  }

  out[o] = '\0';
  if (out_len) *out_len = o;
  return AGENT_OK;
}

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return 26 + (int)(c - 'a');
  if (c >= '0' && c <= '9') return 52 + (int)(c - '0');
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

agent_status_t agent_base64_decode(
  const char* in,
  size_t in_len,
  uint8_t* out,
  size_t out_cap,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!in && in_len != 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!out && out_cap != 0) return AGENT_ERR_INVALID_ARGUMENT;

  // Reject whitespace and URL-safe alphabet to keep behavior predictable.
  for (size_t i = 0; i < in_len; i++) {
    const char c = in[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return AGENT_ERR_INVALID_ARGUMENT;
    if (c == '-' || c == '_') return AGENT_ERR_INVALID_ARGUMENT;
  }

  // Count padding, if any.
  size_t pad = 0;
  if (in_len >= 1 && in[in_len - 1] == '=') pad++;
  if (in_len >= 2 && in[in_len - 2] == '=') pad++;

  // Padding may only appear at the end.
  if (pad) {
    for (size_t i = 0; i + pad < in_len; i++) {
      if (in[i] == '=') return AGENT_ERR_INVALID_ARGUMENT;
    }
  }

  // Accept both padded and unpadded input.
  // Unpadded is allowed only when there are no '='.
  const size_t mod = in_len % 4;
  if (mod == 1) return AGENT_ERR_INVALID_ARGUMENT;
  if (pad != 0 && mod != 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (pad == 0 && mod == 0) {
    // ok
  } else if (pad == 0 && (mod == 2 || mod == 3)) {
    // ok: unpadded tail
  }

  size_t expected = 0;
  if (pad) {
    expected = (in_len / 4) * 3 - pad;
  } else {
    expected = (in_len / 4) * 3;
    if (mod == 2) expected += 1;
    if (mod == 3) expected += 2;
  }
  if (expected > out_cap) return AGENT_ERR_BOUNDS;

  size_t o = 0;
  size_t i = 0;

  // Full 4-char blocks.
  const size_t full_blocks = in_len / 4;
  for (size_t b = 0; b < full_blocks; b++) {
    const char c0 = in[i + 0];
    const char c1 = in[i + 1];
    const char c2 = in[i + 2];
    const char c3 = in[i + 3];
    const int v0 = b64_val(c0);
    const int v1 = b64_val(c1);
    const int v2 = (c2 == '=') ? -2 : b64_val(c2);
    const int v3 = (c3 == '=') ? -2 : b64_val(c3);
    if (v0 < 0 || v1 < 0) return AGENT_ERR_INVALID_ARGUMENT;
    if (v2 < -2 || v3 < -2) return AGENT_ERR_INVALID_ARGUMENT;
    if (v2 == -2 && v3 != -2) return AGENT_ERR_INVALID_ARGUMENT; // "x=" is invalid; must be "=="
    if (v3 == -2 && v2 == -2) {
      // ok: "=="
    } else if (v3 == -2 && v2 < 0) {
      // invalid: c2 cannot be non-base64 when c3 is '='
      return AGENT_ERR_INVALID_ARGUMENT;
    }

    const uint32_t trip =
      ((uint32_t)v0 << 18) |
      ((uint32_t)v1 << 12) |
      ((uint32_t)((v2 >= 0) ? v2 : 0) << 6) |
      (uint32_t)((v3 >= 0) ? v3 : 0);

    out[o++] = (uint8_t)((trip >> 16) & 0xff);
    if (c2 != '=') out[o++] = (uint8_t)((trip >> 8) & 0xff);
    if (c3 != '=') out[o++] = (uint8_t)(trip & 0xff);

    i += 4;
  }

  // Unpadded tail (2 or 3 chars).
  if (pad == 0 && mod) {
    const char c0 = in[i + 0];
    const char c1 = in[i + 1];
    const int v0 = b64_val(c0);
    const int v1 = b64_val(c1);
    if (v0 < 0 || v1 < 0) return AGENT_ERR_INVALID_ARGUMENT;
    if (mod == 2) {
      const uint32_t trip = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
      out[o++] = (uint8_t)((trip >> 16) & 0xff);
    } else if (mod == 3) {
      const char c2 = in[i + 2];
      const int v2 = b64_val(c2);
      if (v2 < 0) return AGENT_ERR_INVALID_ARGUMENT;
      const uint32_t trip = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6);
      out[o++] = (uint8_t)((trip >> 16) & 0xff);
      out[o++] = (uint8_t)((trip >> 8) & 0xff);
    }
  }

  if (o != expected) return AGENT_ERR_INTERNAL;
  if (out_len) *out_len = o;
  return AGENT_OK;
}
