#include "http_util.h"

#include <cctype>
#include <cstring>

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

}  // namespace agentd
