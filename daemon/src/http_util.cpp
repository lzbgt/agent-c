#include "http_util.h"

#include <cctype>
#include <cstring>

#include <json/json.h>

namespace agentd {

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      auto hex = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return 10 + (x - 'a');
        if (x >= 'A' && x <= 'F') return 10 + (x - 'A');
        return -1;
      };
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> query_get(const std::string& query, const std::string& key) {
  size_t start = 0;
  while (start <= query.size()) {
    size_t amp = query.find('&', start);
    if (amp == std::string::npos) amp = query.size();
    const std::string_view part(query.data() + start, amp - start);
    const size_t eq = part.find('=');
    std::string_view k = eq == std::string_view::npos ? part : part.substr(0, eq);
    std::string_view v = eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1);
    if (k == key) {
      return url_decode(v);
    }
    start = amp + 1;
  }
  return std::nullopt;
}

bool string_to_bool(const std::string& s) {
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string content_type_from_path(const std::filesystem::path& p) {
  const std::string ext = p.extension().string();
  auto eqi = [&](const char* s) {
    if (ext.size() != std::strlen(s)) return false;
    for (size_t i = 0; i < ext.size(); i++) {
      if (std::tolower((unsigned char)ext[i]) != std::tolower((unsigned char)s[i])) return false;
    }
    return true;
  };
  if (eqi(".png")) return "image/png";
  if (eqi(".jpg") || eqi(".jpeg")) return "image/jpeg";
  if (eqi(".gif")) return "image/gif";
  if (eqi(".webp")) return "image/webp";
  if (eqi(".svg")) return "image/svg+xml";
  if (eqi(".mp3")) return "audio/mpeg";
  if (eqi(".wav")) return "audio/wav";
  if (eqi(".mp4")) return "video/mp4";
  if (eqi(".webm")) return "video/webm";
  if (eqi(".mov")) return "video/quicktime";
  if (eqi(".pptx")) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  if (eqi(".txt") || eqi(".md")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::string trim_slashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string header_get_ci(const std::map<std::string, std::string>& headers, const std::string& key_lc) {
  auto it = headers.find(key_lc);
  if (it == headers.end()) return "";
  return it->second;
}

std::string bearer_token_from_auth_header(const std::string& auth) {
  // Headers were lowercased at parse time, but values are case-preserved. Accept common "Bearer " prefix.
  const std::string prefix = "bearer ";
  if (auth.size() >= prefix.size()) {
    std::string head = auth.substr(0, prefix.size());
    for (char& c : head) c = (char)std::tolower((unsigned char)c);
    if (head == prefix) {
      return auth.substr(prefix.size());
    }
  }
  return "";
}

static std::string trim_ws(std::string_view s) {
  size_t start = 0;
  size_t end = s.size();
  while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return std::string(s.substr(start, end - start));
}

std::string cookie_get(const std::map<std::string, std::string>& headers, const std::string& name) {
  if (name.empty()) return "";
  const std::string raw = header_get_ci(headers, "cookie");
  if (raw.empty()) return "";
  size_t start = 0;
  while (start <= raw.size()) {
    size_t sep = raw.find(';', start);
    if (sep == std::string::npos) sep = raw.size();
    std::string part = trim_ws(std::string_view(raw.data() + start, sep - start));
    if (!part.empty()) {
      const size_t eq = part.find('=');
      if (eq != std::string::npos) {
        std::string k = trim_ws(std::string_view(part.data(), eq));
        if (k == name) {
          std::string v = trim_ws(std::string_view(part.data() + eq + 1, part.size() - eq - 1));
          if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
          }
          return v;
        }
      }
    }
    if (sep == raw.size()) break;
    start = sep + 1;
  }
  return "";
}

std::string error_code_from_message(const std::string& message) {
  std::string out;
  out.reserve(message.size());
  bool prev_us = false;
  for (unsigned char c : message) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out.push_back((char)c);
      prev_us = false;
      continue;
    }
    if (c >= 'A' && c <= 'Z') {
      out.push_back((char)(c - 'A' + 'a'));
      prev_us = false;
      continue;
    }
    if (!prev_us && !out.empty()) {
      out.push_back('_');
      prev_us = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  if (out.empty()) out = "error";
  return out;
}

std::string json_error_body(const std::string& message, const std::string& code, const Json::Value* details) {
  Json::Value root(Json::objectValue);
  root["ok"] = false;
  root["error"] = message;
  root["err"] = message;
  const std::string final_code = code.empty() ? error_code_from_message(message) : code;
  if (!final_code.empty()) root["code"] = final_code;
  if (details) root["details"] = *details;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, root);
}

}  // namespace agentd
