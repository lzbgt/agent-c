#include "cbor_encode.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace agentd {
namespace {

static bool fail(std::string* out_error, const std::string& msg) {
  if (out_error) *out_error = msg;
  return false;
}

static void append_u8(std::string* out, uint8_t b) {
  out->push_back((char)b);
}

static void append_be_u16(std::string* out, uint16_t v) {
  append_u8(out, (uint8_t)((v >> 8) & 0xff));
  append_u8(out, (uint8_t)(v & 0xff));
}

static void append_be_u32(std::string* out, uint32_t v) {
  append_u8(out, (uint8_t)((v >> 24) & 0xff));
  append_u8(out, (uint8_t)((v >> 16) & 0xff));
  append_u8(out, (uint8_t)((v >> 8) & 0xff));
  append_u8(out, (uint8_t)(v & 0xff));
}

static void append_be_u64(std::string* out, uint64_t v) {
  for (int i = 7; i >= 0; i--) {
    append_u8(out, (uint8_t)((v >> (i * 8)) & 0xff));
  }
}

static void append_ai_u64(std::string* out, uint8_t major, uint64_t n) {
  // major: already shifted (major<<5).
  if (n < 24) {
    append_u8(out, (uint8_t)(major | (uint8_t)n));
  } else if (n <= 0xff) {
    append_u8(out, (uint8_t)(major | 24));
    append_u8(out, (uint8_t)n);
  } else if (n <= 0xffff) {
    append_u8(out, (uint8_t)(major | 25));
    append_be_u16(out, (uint16_t)n);
  } else if (n <= 0xffffffffULL) {
    append_u8(out, (uint8_t)(major | 26));
    append_be_u32(out, (uint32_t)n);
  } else {
    append_u8(out, (uint8_t)(major | 27));
    append_be_u64(out, n);
  }
}

static bool encode_any(const Json::Value& v, std::string* out, std::string* out_error, size_t depth);

static bool encode_text(const std::string& s, std::string* out) {
  const uint64_t n = (uint64_t)s.size();
  append_ai_u64(out, /*major=*/(3u << 5), n);
  out->append(s);
  return true;
}

static bool encode_array(const Json::Value& v, std::string* out, std::string* out_error, size_t depth) {
  if (!v.isArray()) return fail(out_error, "cbor: expected array");
  const uint64_t n = (uint64_t)v.size();
  append_ai_u64(out, /*major=*/(4u << 5), n);
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (!encode_any(v[i], out, out_error, depth + 1)) return false;
  }
  return true;
}

static bool encode_object(const Json::Value& v, std::string* out, std::string* out_error, size_t depth) {
  if (!v.isObject()) return fail(out_error, "cbor: expected object");
  const auto names = v.getMemberNames();
  std::vector<std::string> keys(names.begin(), names.end());
  // Deterministic map key ordering (text strings):
  // - sort by UTF-8 byte length
  // - then lexicographically by UTF-8 bytes
  //
  // This is compatible with RFC 8949 deterministic encoding requirements for
  // text-string keys, without implementing full “sort by encoded bytes” for all
  // possible CBOR key types (we restrict envelope keys to text strings anyway).
  std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
  });

  append_ai_u64(out, /*major=*/(5u << 5), (uint64_t)keys.size());
  for (const auto& k : keys) {
    (void)encode_text(k, out);
    if (!encode_any(v[k], out, out_error, depth + 1)) return false;
  }
  return true;
}

static bool encode_any(const Json::Value& v, std::string* out, std::string* out_error, size_t depth) {
  if (!out) return fail(out_error, "cbor: missing out");
  if (depth > 64) return fail(out_error, "cbor: max depth exceeded");

  if (v.isNull()) {
    append_u8(out, 0xf6);
    return true;
  }
  if (v.isBool()) {
    append_u8(out, v.asBool() ? 0xf5 : 0xf4);
    return true;
  }
  // IMPORTANT: preserve JSON number *types* when encoding to CBOR.
  //
  // JsonCpp's `isDouble()` / `isInt64()` / `isUInt64()` are *conversion* checks
  // (i.e. "can this value be represented as ..."), not strict type tests.
  // For deterministic CBOR we must:
  // - encode Json::realValue as float64 (0xfb) even if numerically integral
  // - encode integer types as integers (major 0/1), never as floats
  if (v.type() == Json::realValue) {
    // Encode as float64 (major 7, ai=27) for simplicity and determinism.
    append_u8(out, 0xfb);
    const double d = v.asDouble();
    uint64_t bits = 0;
    static_assert(sizeof(double) == 8, "double must be 64-bit");
    std::memcpy(&bits, &d, 8);
    append_be_u64(out, bits);
    return true;
  }
  if (v.isInt64()) {
    const int64_t x = v.asInt64();
    if (x >= 0) {
      append_ai_u64(out, /*major=*/(0u << 5), (uint64_t)x);
      return true;
    }
    // negative: major 1 uses (-1 - n)
    const uint64_t n = (uint64_t)(-1 - x);
    append_ai_u64(out, /*major=*/(1u << 5), n);
    return true;
  }
  if (v.isUInt64()) {
    append_ai_u64(out, /*major=*/(0u << 5), v.asUInt64());
    return true;
  }
  if (v.isInt()) {
    const int x = v.asInt();
    if (x >= 0) append_ai_u64(out, /*major=*/(0u << 5), (uint64_t)x);
    else append_ai_u64(out, /*major=*/(1u << 5), (uint64_t)(-1 - (int64_t)x));
    return true;
  }
  if (v.isUInt()) {
    append_ai_u64(out, /*major=*/(0u << 5), (uint64_t)v.asUInt());
    return true;
  }
  if (v.isString()) {
    return encode_text(v.asString(), out);
  }
  if (v.isArray()) {
    return encode_array(v, out, out_error, depth);
  }
  if (v.isObject()) {
    return encode_object(v, out, out_error, depth);
  }

  return fail(out_error, "cbor: unsupported Json::Value type");
}

}  // namespace

bool cbor_encode_json_value(
  const Json::Value& v,
  std::string* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return fail(out_error, "cbor: missing out");
  out->clear();
  out->reserve(256);

  if (!encode_any(v, out, out_error, /*depth=*/0)) return false;
  if (out->size() > (size_t)(4 * 1024 * 1024)) return fail(out_error, "cbor: encoded body too large");
  return true;
}

}  // namespace agentd
