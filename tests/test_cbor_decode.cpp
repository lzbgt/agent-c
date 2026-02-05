#include "cbor_decode.h"
#include "cbor_encode.h"

#include <cassert>
#include <string>

static std::string cbor_text(const std::string& s) {
  // Definite-length text strings only, length < 24.
  assert(s.size() < 24);
  std::string out;
  out.push_back((char)(0x60 | (uint8_t)s.size()));
  out.append(s);
  return out;
}

static std::string cbor_uint(uint64_t u) {
  // Only encode small uints for this test.
  assert(u < 24);
  std::string out;
  out.push_back((char)(0x00 | (uint8_t)u));
  return out;
}

static std::string cbor_map_1(const std::string& k, const std::string& v) {
  // map(1) { k: v }
  std::string out;
  out.push_back((char)(0xa0 | 1));
  out += cbor_text(k);
  out += cbor_text(v);
  return out;
}

int main() {
  using agentd::cbor_decode_to_json_value;
  using agentd::cbor_encode_json_value;

  {
    Json::Value out;
    std::string err;
    const bool ok = cbor_decode_to_json_value("", &out, &err);
    assert(!ok);
  }

  {
    // {"type":"NODE_HELLO"}
    const std::string b = cbor_map_1("type", "NODE_HELLO");
    Json::Value out;
    std::string err;
    const bool ok = cbor_decode_to_json_value(b, &out, &err);
    assert(ok);
    assert(out.isObject());
    assert(out.isMember("type") && out["type"].isString());
    assert(out["type"].asString() == "NODE_HELLO");
  }

  {
    // Encoder emits deterministic, definite-length CBOR that the decoder can round-trip.
    Json::Value v(Json::objectValue);
    v["ok"] = true;
    v["type"] = "NODE_HELLO";
    v["n"] = (Json::Int64)123;
    Json::Value arr(Json::arrayValue);
    arr.append("a");
    arr.append("b");
    v["arr"] = arr;

    std::string cbor;
    std::string err;
    assert(cbor_encode_json_value(v, &cbor, &err));

    Json::Value out;
    std::string derr;
    assert(cbor_decode_to_json_value(cbor, &out, &derr));
    assert(out.isObject());
    assert(out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool());
    assert(out.isMember("type") && out["type"].isString() && out["type"].asString() == "NODE_HELLO");
    assert(out.isMember("n") && (out["n"].isInt64() || out["n"].isInt() || out["n"].isUInt64() || out["n"].isUInt()));
    assert(out.isMember("arr") && out["arr"].isArray() && out["arr"].size() == 2);
  }

  {
    // Trailing bytes should be rejected.
    const std::string b = cbor_map_1("k", "v") + cbor_uint(1);
    Json::Value out;
    std::string err;
    const bool ok = cbor_decode_to_json_value(b, &out, &err);
    assert(!ok);
  }

  {
    // Indefinite-length map should be rejected.
    // 0xbf ... 0xff
    const std::string b = std::string("\xbf", 1) + cbor_text("k") + cbor_text("v") + std::string("\xff", 1);
    Json::Value out;
    std::string err;
    const bool ok = cbor_decode_to_json_value(b, &out, &err);
    assert(!ok);
  }

  {
    // Non-text map key should be rejected:
    // { 1: "x" }
    const std::string b = std::string("\xa1", 1) + cbor_uint(1) + cbor_text("x");
    Json::Value out;
    std::string err;
    const bool ok = cbor_decode_to_json_value(b, &out, &err);
    assert(!ok);
  }

  return 0;
}
