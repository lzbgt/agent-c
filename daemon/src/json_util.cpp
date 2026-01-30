#include "json_util.h"

#include <sstream>

namespace agentd {

std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

bool json_get_u64_nonneg(const Json::Value& obj, const char* key, uint64_t* out) {
  if (!out) return false;
  if (!obj.isObject() || !key || !key[0]) return false;
  if (!obj.isMember(key)) return false;
  const Json::Value& v = obj[key];
  if (v.isUInt64()) {
    *out = v.asUInt64();
    return true;
  }
  if (v.isInt64()) {
    const Json::Int64 x = v.asInt64();
    if (x < 0) return false;
    *out = (uint64_t)x;
    return true;
  }
  if (v.isUInt()) {
    *out = (uint64_t)v.asUInt();
    return true;
  }
  if (v.isInt()) {
    const int x = v.asInt();
    if (x < 0) return false;
    *out = (uint64_t)x;
    return true;
  }
  if (v.isDouble()) {
    const double x = v.asDouble();
    if (!(x >= 0.0)) return false;
    *out = (uint64_t)x;
    return true;
  }
  return false;
}

bool json_parse_object(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  *out = v;
  return true;
}

bool json_parse_any(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  *out = v;
  return true;
}

std::string json_try_extract_assistant_content_from_completion(const Json::Value& root) {
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return "";
  const auto& msg = choices[0]["message"];
  const auto& content = msg["content"];
  if (content.isString()) return content.asString();
  const auto& text = choices[0]["text"];
  if (text.isString()) return text.asString();
  return "";
}

}  // namespace agentd

