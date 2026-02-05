#include "agent/edge_interop.h"

#include <string.h>

static int umbmp_char_ok(char c) {
  const int ok =
    (c >= 'a' && c <= 'z') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') ||
    (c == '-') || (c == '_') || (c == '.') || (c == ':');
  return ok ? 1 : 0;
}

int agent_umbmp_id_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0 || len > AGENT_UM_BMP_MAX_ID_LEN) return 0;
  for (size_t i = 0; i < len; i++) {
    if (!umbmp_char_ok(s[i])) return 0;
  }
  return 1;
}

agent_status_t agent_umbmp_sanitize_id_token(
  const char* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t max_len,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';

  const size_t cap_len = out_cap - 1; // reserve NUL
  if (cap_len == 0) return AGENT_ERR_BOUNDS;

  if (max_len == 0 || max_len > cap_len) max_len = cap_len;
  size_t n = in_len;
  if (n > max_len) n = max_len;

  // Map invalid characters to '_' and truncate.
  for (size_t i = 0; i < n; i++) {
    const char c = in ? in[i] : '\0';
    out[i] = umbmp_char_ok(c) ? c : '_';
  }

  // Trim leading/trailing underscores.
  size_t start = 0;
  while (start < n && out[start] == '_') start++;
  size_t end = n;
  while (end > start && out[end - 1] == '_') end--;

  if (end <= start) {
    // Default token.
    static const char* kDefault = "msg";
    const size_t kDefaultLen = 3;
    if (out_cap <= kDefaultLen) return AGENT_ERR_BOUNDS;
    memcpy(out, kDefault, kDefaultLen);
    out[kDefaultLen] = '\0';
    if (out_len) *out_len = kDefaultLen;
    return AGENT_OK;
  }

  const size_t trimmed_len = end - start;
  if (start != 0) memmove(out, out + start, trimmed_len);
  out[trimmed_len] = '\0';
  if (out_len) *out_len = trimmed_len;
  return AGENT_OK;
}

