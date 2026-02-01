#include "string_util.h"

#include <cctype>

namespace agentd {

std::string lower_copy(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool url_contains_ci(const std::string& url, const std::string& needle) {
  if (needle.empty()) return false;
  const std::string u = lower_copy(url);
  const std::string n = lower_copy(needle);
  return u.find(n) != std::string::npos;
}

std::string trim_copy(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace((unsigned char)s[start])) start++;
  if (start == s.size()) return "";
  size_t end = s.size();
  while (end > start && std::isspace((unsigned char)s[end - 1])) end--;
  return s.substr(start, end - start);
}

std::string truncate_for_event(const std::string& s, size_t max_bytes, bool* out_truncated) {
  if (out_truncated) *out_truncated = false;
  if (max_bytes == 0 || s.size() <= max_bytes) {
    return s;
  }
  if (out_truncated) *out_truncated = true;
  std::string out = s.substr(0, max_bytes);
  out += "...(truncated)";
  return out;
}

std::string sanitize_filename_component_ascii(std::string s, const std::string& fallback) {
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    out.push_back(ok ? (char)c : '_');
    if (out.size() >= 200) break;
  }
  if (out.empty()) out = fallback.empty() ? "default" : fallback;
  if (!out.empty() && out[0] == '.') out[0] = '_';
  if (out == "." || out == "..") out = fallback.empty() ? "default" : fallback;
  // Avoid accidental traversal-ish strings like "..foo".
  if (out.find("..") != std::string::npos) {
    for (size_t i = 0; i + 1 < out.size(); i++) {
      if (out[i] == '.' && out[i + 1] == '.') {
        out[i] = '_';
        out[i + 1] = '_';
      }
    }
  }
  return out;
}

}  // namespace agentd
