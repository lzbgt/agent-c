#include "cbor_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace agentd {
namespace {

struct CborCursor {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;
  size_t depth = 0;
};

static bool fail(std::string* out_error, const std::string& msg) {
  if (out_error) *out_error = msg;
  return false;
}

static bool ensure(CborCursor* c, size_t n, std::string* out_error) {
  if (!c || !c->p || !c->end) return fail(out_error, "cbor: invalid cursor");
  const size_t rem = (size_t)(c->end - c->p);
  if (rem < n) return fail(out_error, "cbor: truncated");
  return true;
}

static bool read_u8(CborCursor* c, uint8_t* out, std::string* out_error) {
  if (!ensure(c, 1, out_error)) return false;
  if (out) *out = *c->p;
  c->p++;
  return true;
}

static bool read_be_u16(CborCursor* c, uint16_t* out, std::string* out_error) {
  if (!ensure(c, 2, out_error)) return false;
  const uint16_t v = (uint16_t)((uint16_t)c->p[0] << 8) | (uint16_t)c->p[1];
  if (out) *out = v;
  c->p += 2;
  return true;
}

static bool read_be_u32(CborCursor* c, uint32_t* out, std::string* out_error) {
  if (!ensure(c, 4, out_error)) return false;
  const uint32_t v =
    ((uint32_t)c->p[0] << 24) |
    ((uint32_t)c->p[1] << 16) |
    ((uint32_t)c->p[2] << 8) |
    (uint32_t)c->p[3];
  if (out) *out = v;
  c->p += 4;
  return true;
}

static bool read_be_u64(CborCursor* c, uint64_t* out, std::string* out_error) {
  if (!ensure(c, 8, out_error)) return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | (uint64_t)c->p[i];
  }
  if (out) *out = v;
  c->p += 8;
  return true;
}

static bool read_ai_u64(CborCursor* c, uint8_t ai, uint64_t* out, std::string* out_error) {
  if (!c) return fail(out_error, "cbor: invalid cursor");
  if (ai < 24) {
    if (out) *out = (uint64_t)ai;
    return true;
  }
  if (ai == 24) {
    uint8_t b = 0;
    if (!read_u8(c, &b, out_error)) return false;
    if (out) *out = (uint64_t)b;
    return true;
  }
  if (ai == 25) {
    uint16_t v = 0;
    if (!read_be_u16(c, &v, out_error)) return false;
    if (out) *out = (uint64_t)v;
    return true;
  }
  if (ai == 26) {
    uint32_t v = 0;
    if (!read_be_u32(c, &v, out_error)) return false;
    if (out) *out = (uint64_t)v;
    return true;
  }
  if (ai == 27) {
    uint64_t v = 0;
    if (!read_be_u64(c, &v, out_error)) return false;
    if (out) *out = v;
    return true;
  }
  if (ai == 31) return fail(out_error, "cbor: indefinite length not supported");
  return fail(out_error, "cbor: reserved additional info");
}

static double half_to_double(uint16_t h) {
  // IEEE 754 binary16 -> double (best-effort).
  const int sign = (h >> 15) & 1;
  const int exp = (h >> 10) & 0x1f;
  const int frac = h & 0x3ff;
  if (exp == 0) {
    if (frac == 0) return sign ? -0.0 : 0.0;
    // subnormal: frac * 2^-24
    const double v = (double)frac / (double)(1 << 10);
    const double r = std::ldexp(v, -14);
    return sign ? -r : r;
  }
  if (exp == 31) {
    if (frac == 0) return sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double v = 1.0 + ((double)frac / (double)(1 << 10));
  const double r = std::ldexp(v, exp - 15);
  return sign ? -r : r;
}

static bool decode_item(CborCursor* c, Json::Value* out, std::string* out_error);

static bool decode_text(CborCursor* c, uint64_t n, Json::Value* out, std::string* out_error) {
  if (!ensure(c, (size_t)n, out_error)) return false;
  if (n > (uint64_t)std::numeric_limits<size_t>::max()) return fail(out_error, "cbor: text too large");
  const char* s = (const char*)c->p;
  std::string str(s, s + (size_t)n);
  c->p += (size_t)n;
  if (out) *out = Json::Value(str);
  return true;
}

static bool decode_bytes(CborCursor* c, uint64_t n, Json::Value* out, std::string* out_error) {
  // For UM-BMP envelopes we don't expect byte strings. Reject to avoid ambiguous JSON representation.
  (void)out;
  if (!ensure(c, (size_t)n, out_error)) return false;
  c->p += (size_t)n;
  return fail(out_error, "cbor: byte strings not supported (expected text strings)");
}

static bool decode_array(CborCursor* c, uint64_t n, Json::Value* out, std::string* out_error) {
  if (!c) return fail(out_error, "cbor: invalid cursor");
  if (n > 100000) return fail(out_error, "cbor: array too large");
  if (c->depth >= 64) return fail(out_error, "cbor: max depth exceeded");
  c->depth++;

  Json::Value arr(Json::arrayValue);
  for (uint64_t i = 0; i < n; i++) {
    Json::Value v;
    if (!decode_item(c, &v, out_error)) {
      c->depth--;
      return false;
    }
    arr.append(v);
  }
  c->depth--;
  if (out) *out = arr;
  return true;
}

static bool decode_map(CborCursor* c, uint64_t n, Json::Value* out, std::string* out_error) {
  if (!c) return fail(out_error, "cbor: invalid cursor");
  if (n > 100000) return fail(out_error, "cbor: map too large");
  if (c->depth >= 64) return fail(out_error, "cbor: max depth exceeded");
  c->depth++;

  Json::Value obj(Json::objectValue);
  for (uint64_t i = 0; i < n; i++) {
    // Keys must be text strings for this profile.
    uint8_t ib = 0;
    if (!read_u8(c, &ib, out_error)) {
      c->depth--;
      return false;
    }
    const uint8_t major = (ib >> 5) & 0x7;
    const uint8_t ai = ib & 0x1f;
    if (major != 3) {
      c->depth--;
      return fail(out_error, "cbor: map key must be text string");
    }
    uint64_t klen = 0;
    if (!read_ai_u64(c, ai, &klen, out_error)) {
      c->depth--;
      return false;
    }
    Json::Value k;
    if (!decode_text(c, klen, &k, out_error)) {
      c->depth--;
      return false;
    }
    const std::string key = k.isString() ? k.asString() : "";
    if (key.empty()) {
      c->depth--;
      return fail(out_error, "cbor: empty key");
    }

    Json::Value v;
    if (!decode_item(c, &v, out_error)) {
      c->depth--;
      return false;
    }
    obj[key] = v;
  }

  c->depth--;
  if (out) *out = obj;
  return true;
}

static bool decode_item(CborCursor* c, Json::Value* out, std::string* out_error) {
  if (!c) return fail(out_error, "cbor: invalid cursor");
  uint8_t ib = 0;
  if (!read_u8(c, &ib, out_error)) return false;

  const uint8_t major = (ib >> 5) & 0x7;
  const uint8_t ai = ib & 0x1f;

  if (major == 0) {
    uint64_t u = 0;
    if (!read_ai_u64(c, ai, &u, out_error)) return false;
    if (u <= (uint64_t)std::numeric_limits<Json::UInt64>::max()) {
      if (out) *out = Json::Value((Json::UInt64)u);
      return true;
    }
    return fail(out_error, "cbor: uint too large");
  }
  if (major == 1) {
    uint64_t u = 0;
    if (!read_ai_u64(c, ai, &u, out_error)) return false;
    // Value is -1 - u.
    if (u > (uint64_t)std::numeric_limits<int64_t>::max()) {
      return fail(out_error, "cbor: negative int too large");
    }
    const int64_t v = -1 - (int64_t)u;
    if (out) *out = Json::Value((Json::Int64)v);
    return true;
  }
  if (major == 2) {
    uint64_t n = 0;
    if (!read_ai_u64(c, ai, &n, out_error)) return false;
    return decode_bytes(c, n, out, out_error);
  }
  if (major == 3) {
    uint64_t n = 0;
    if (!read_ai_u64(c, ai, &n, out_error)) return false;
    return decode_text(c, n, out, out_error);
  }
  if (major == 4) {
    uint64_t n = 0;
    if (!read_ai_u64(c, ai, &n, out_error)) return false;
    return decode_array(c, n, out, out_error);
  }
  if (major == 5) {
    uint64_t n = 0;
    if (!read_ai_u64(c, ai, &n, out_error)) return false;
    return decode_map(c, n, out, out_error);
  }
  if (major == 6) {
    // Tag: ignore and decode the following item.
    uint64_t tag = 0;
    if (!read_ai_u64(c, ai, &tag, out_error)) return false;
    (void)tag;
    return decode_item(c, out, out_error);
  }
  if (major == 7) {
    // Simple / floats.
    if (ai == 20) {
      if (out) *out = Json::Value(false);
      return true;
    }
    if (ai == 21) {
      if (out) *out = Json::Value(true);
      return true;
    }
    if (ai == 22) {
      if (out) *out = Json::Value(Json::nullValue);
      return true;
    }
    if (ai == 23) {
      if (out) *out = Json::Value(Json::nullValue);
      return true;
    }
    if (ai == 25) {
      uint16_t h = 0;
      if (!read_be_u16(c, &h, out_error)) return false;
      if (out) *out = Json::Value(half_to_double(h));
      return true;
    }
    if (ai == 26) {
      uint32_t w = 0;
      if (!read_be_u32(c, &w, out_error)) return false;
      float f = 0.0f;
      static_assert(sizeof(float) == 4, "float must be 32-bit");
      std::memcpy(&f, &w, 4);
      if (out) *out = Json::Value((double)f);
      return true;
    }
    if (ai == 27) {
      uint64_t w = 0;
      if (!read_be_u64(c, &w, out_error)) return false;
      double d = 0.0;
      static_assert(sizeof(double) == 8, "double must be 64-bit");
      std::memcpy(&d, &w, 8);
      if (out) *out = Json::Value(d);
      return true;
    }
    if (ai == 24) {
      uint8_t simple = 0;
      if (!read_u8(c, &simple, out_error)) return false;
      (void)simple;
      return fail(out_error, "cbor: unsupported simple value");
    }
    return fail(out_error, "cbor: unsupported simple/float");
  }

  return fail(out_error, "cbor: unknown major type");
}

}  // namespace

bool cbor_decode_to_json_value(
  const std::string& cbor_bytes,
  Json::Value* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return fail(out_error, "cbor: missing out");
  *out = Json::Value(Json::nullValue);

  if (cbor_bytes.empty()) return fail(out_error, "cbor: empty body");
  if (cbor_bytes.size() > (size_t)(1024 * 1024)) return fail(out_error, "cbor: body too large");

  CborCursor c;
  c.p = (const uint8_t*)cbor_bytes.data();
  c.end = (const uint8_t*)cbor_bytes.data() + cbor_bytes.size();
  c.depth = 0;

  Json::Value v;
  if (!decode_item(&c, &v, out_error)) return false;

  // Reject trailing bytes. Canonical encoders should not leave extra data.
  if (c.p != c.end) {
    return fail(out_error, "cbor: trailing bytes");
  }

  *out = v;
  return true;
}

}  // namespace agentd
