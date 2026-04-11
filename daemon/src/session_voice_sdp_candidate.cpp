#include "session_voice_sdp_candidate.h"

#include <cctype>
#include <sstream>
#include <vector>

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

std::string lower_copy_local(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::vector<std::string> split_space_tokens(const std::string& value) {
  std::istringstream stream(value);
  std::vector<std::string> out;
  std::string token;
  while (stream >> token) out.push_back(token);
  return out;
}

}  // namespace

bool sdp_candidate_is_end_marker(const std::string& value) {
  const std::string trimmed = trim_copy_local(value);
  return trimmed.empty() ||
         trimmed == "a=end-of-candidates" ||
         trimmed == "end-of-candidates";
}

bool sdp_candidate_is_relay_candidate(const std::string& value) {
  const std::string trimmed = trim_copy_local(value);
  if (!starts_with(trimmed, "a=candidate:") &&
      !starts_with(trimmed, "candidate:")) {
    return false;
  }
  const std::vector<std::string> tokens = split_space_tokens(trimmed);
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (lower_copy_local(tokens[i]) == "typ" &&
        lower_copy_local(tokens[i + 1]) == "relay") {
      return true;
    }
  }
  return false;
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

std::string strip_sdp_relay_candidate_lines(const std::string& sdp) {
  std::string out;
  size_t start = 0;
  while (start < sdp.size()) {
    const size_t newline = sdp.find('\n', start);
    const size_t end = newline == std::string::npos ? sdp.size() : newline + 1;
    const std::string segment = sdp.substr(start, end - start);
    std::string line = segment;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (!sdp_candidate_is_relay_candidate(line)) out += segment;
    if (newline == std::string::npos) break;
    start = end;
  }
  return out;
}

}  // namespace agentd
