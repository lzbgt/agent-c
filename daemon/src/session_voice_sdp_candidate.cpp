#include "session_voice_sdp_candidate.h"

#include <cctype>

namespace agentd {
namespace {

std::string trim_copy_local(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    begin += 1;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end -= 1;
  }
  return value.substr(begin, end - begin);
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

}  // namespace

bool sdp_candidate_is_end_marker(const std::string& value) {
  const std::string trimmed = trim_copy_local(value);
  return trimmed.empty() ||
         trimmed == "a=end-of-candidates" ||
         trimmed == "end-of-candidates";
}

std::string normalize_sdp_candidate_line(const std::string& value) {
  const std::string trimmed = trim_copy_local(value);
  if (trimmed.empty()) return "";
  if (trimmed == "a=end-of-candidates") return "a=end-of-candidates";
  if (trimmed == "end-of-candidates") return "a=end-of-candidates";
  if (!starts_with(trimmed, "a=candidate:") && !starts_with(trimmed, "candidate:")) {
    return trimmed;
  }

  std::string out = starts_with(trimmed, "candidate:") ? "a=" + trimmed : trimmed;
  const size_t foundation_end = out.find(' ');
  if (foundation_end == std::string::npos) return out;
  const size_t component_begin = foundation_end + 1;
  const size_t component_end = out.find(' ', component_begin);
  if (component_end == std::string::npos) return out;
  const size_t transport_begin = component_end + 1;
  const size_t transport_end = out.find(' ', transport_begin);
  if (transport_end == std::string::npos) return out;

  for (size_t i = transport_begin; i < transport_end; ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

}  // namespace agentd
