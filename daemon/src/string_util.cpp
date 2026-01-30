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

}  // namespace agentd

