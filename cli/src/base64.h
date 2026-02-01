#pragma once

#include <cstddef>
#include <string>

// Minimal base64 helpers (host-only).
// - Uses standard Base64 alphabet (RFC 4648, not URL-safe).
// - Ignores ASCII whitespace on decode.

std::string base64_encode(const void* data, size_t len);

// Decodes a standard base64 string into raw bytes.
// Returns true on success; on failure, returns false and sets out_error.
bool base64_decode(const std::string& b64, std::string* out_bytes, std::string* out_error);

