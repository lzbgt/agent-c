#include "json_util.h"

#include <cstdlib>
#include <cctype>
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

bool json_pointer_get(const Json::Value& root, const std::string& ptr, const Json::Value** out) {
  if (!out) return false;
  *out = nullptr;
  if (ptr.empty()) {
    *out = &root;
    return true;
  }
  if (ptr[0] != '/') return false;
  const Json::Value* cur = &root;

  size_t i = 1;
  while (i <= ptr.size()) {
    size_t slash = ptr.find('/', i);
    if (slash == std::string::npos) slash = ptr.size();
    std::string seg = ptr.substr(i, slash - i);
    // unescape ~0 and ~1
    for (size_t j = 0; j + 1 < seg.size(); j++) {
      if (seg[j] == '~') {
        if (seg[j + 1] == '0') {
          seg.replace(j, 2, "~");
        } else if (seg[j + 1] == '1') {
          seg.replace(j, 2, "/");
        }
      }
    }

    if (cur->isObject()) {
      if (!cur->isMember(seg)) return false;
      cur = &((*cur)[seg]);
    } else if (cur->isArray()) {
      if (seg.empty()) return false;
      char* endp = nullptr;
      long long idx = std::strtoll(seg.c_str(), &endp, 10);
      if (!endp || *endp != '\0') return false;
      if (idx < 0) return false;
      if ((Json::ArrayIndex)idx >= cur->size()) return false;
      cur = &((*cur)[(Json::ArrayIndex)idx]);
    } else {
      return false;
    }

    if (slash == ptr.size()) break;
    i = slash + 1;
  }

  *out = cur;
  return true;
}

bool json_value_to_double_best_effort(const Json::Value& v, double* out) {
  if (!out) return false;
  *out = 0.0;
  if (v.isNumeric()) {
    *out = v.asDouble();
    return true;
  }
  if (v.isString()) {
    auto trim_ascii = [](std::string s) -> std::string {
      size_t a = 0;
      while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
      size_t b = s.size();
      while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
      return s.substr(a, b - a);
    };
    const std::string s = trim_ascii(v.asString());
    if (s.empty()) return false;
    char* endp = nullptr;
    const double d = std::strtod(s.c_str(), &endp);
    if (!endp || *endp != '\0') return false;
    *out = d;
    return true;
  }
  return false;
}

}  // namespace agentd
