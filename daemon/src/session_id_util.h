#pragma once

#include <string>

namespace agentd {

inline bool session_id_is_safe(const std::string& s) {
  if (s.empty()) return false;
  if (s.size() > 200) return false;
  // Session ids become filenames; reject path traversal and separators.
  if (s.find('/') != std::string::npos) return false;
  if (s.find('\\') != std::string::npos) return false;
  if (s == "." || s == "..") return false;
  if (s.find("..") != std::string::npos) return false;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.') {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace agentd

