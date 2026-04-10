#include "session_voice_builtin_sdp_answer.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace agentd {
namespace {

std::string trim_copy(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    begin += 1;
  }
  size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    end -= 1;
  }
  return value.substr(begin, end - begin);
}

std::vector<std::string> split_sdp_lines(const std::string& sdp) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < sdp.size()) {
    size_t end = sdp.find('\n', start);
    std::string line = end == std::string::npos ? sdp.substr(start) : sdp.substr(start, end - start);
    line = trim_copy(line);
    if (!line.empty()) out.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

std::string join_sdp_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) {
    out += line;
    out += "\r\n";
  }
  return out;
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string normalize_candidate_line_for_browser_sdp(const std::string& line) {
  if (!starts_with(line, "a=candidate:") && !starts_with(line, "candidate:")) return line;
  std::string out = starts_with(line, "candidate:") ? "a=" + line : line;
  const size_t foundation_end = out.find(' ');
  if (foundation_end == std::string::npos) return line;
  const size_t component_begin = foundation_end + 1;
  const size_t component_end = out.find(' ', component_begin);
  if (component_end == std::string::npos) return line;
  const size_t transport_begin = component_end + 1;
  const size_t transport_end = out.find(' ', transport_begin);
  if (transport_end == std::string::npos) return line;

  for (size_t i = transport_begin; i < transport_end; ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

std::vector<std::string> local_ice_lines_from_description(const std::string& sdp) {
  std::vector<std::string> out;
  bool has_ice_credentials = false;
  for (const auto& line : split_sdp_lines(sdp)) {
    if (starts_with(line, "a=ice-ufrag:") ||
        starts_with(line, "a=ice-pwd:")) {
      out.push_back(line);
      has_ice_credentials = true;
    } else if (starts_with(line, "a=candidate:") || starts_with(line, "candidate:")) {
      out.push_back(normalize_candidate_line_for_browser_sdp(line));
    }
  }
  if (has_ice_credentials) {
    auto insert_pos = out.begin();
    while (insert_pos != out.end() &&
           (starts_with(*insert_pos, "a=ice-ufrag:") ||
            starts_with(*insert_pos, "a=ice-pwd:"))) {
      ++insert_pos;
    }
    out.insert(insert_pos, "a=ice-options:trickle");
  }
  return out;
}

std::string rewrite_mline_for_ice_answer(const std::string& line) {
  if (!starts_with(line, "m=")) return line;
  std::vector<std::string> parts;
  size_t start = 2;
  while (start <= line.size()) {
    const size_t next = line.find(' ', start);
    parts.push_back(next == std::string::npos ? line.substr(start) : line.substr(start, next - start));
    if (next == std::string::npos) break;
    start = next + 1;
  }
  if (parts.size() >= 2) parts[1] = "9";
  std::string out = "m=";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.push_back(' ');
    out += parts[i];
  }
  return out;
}

bool should_copy_session_attribute(const std::string& line) {
  return starts_with(line, "a=group:") || starts_with(line, "a=msid-semantic:");
}

std::string media_kind_from_mline(const std::string& line) {
  if (!starts_with(line, "m=")) return "";
  const size_t end = line.find(' ', 2);
  std::string kind = end == std::string::npos
    ? line.substr(2)
    : line.substr(2, end - 2);
  std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return kind;
}

bool should_copy_media_attribute(const std::string& line) {
  return starts_with(line, "a=mid:") ||
         starts_with(line, "a=rtcp-mux") ||
         starts_with(line, "a=rtcp-rsize") ||
         starts_with(line, "a=rtcp-mux-only") ||
         starts_with(line, "a=rtpmap:") ||
         starts_with(line, "a=fmtp:") ||
         starts_with(line, "a=rtcp-fb:") ||
         starts_with(line, "a=extmap:") ||
         starts_with(line, "a=extmap-allow-mixed") ||
         starts_with(line, "a=sctp-port:") ||
         starts_with(line, "a=max-message-size:");
}

bool is_media_direction_attribute(const std::string& line) {
  return line == "a=sendrecv" ||
         line == "a=sendonly" ||
         line == "a=recvonly" ||
         line == "a=inactive";
}

std::string answer_direction_for_media_lines(const std::vector<std::string>& media_lines) {
  std::string offer_direction = "sendrecv";
  for (const auto& line : media_lines) {
    if (is_media_direction_attribute(line)) {
      offer_direction = line.substr(2);
      break;
    }
  }
  if (offer_direction == "sendonly") return "recvonly";
  if (offer_direction == "recvonly") return "sendonly";
  if (offer_direction == "inactive") return "inactive";
  return "sendrecv";
}

std::string non_empty_or_default(const std::string& value, const char* fallback) {
  const std::string trimmed = trim_copy(value);
  return trimmed.empty() ? std::string(fallback) : trimmed;
}

bool answer_direction_sends_media(const std::string& direction) {
  return direction == "sendrecv" || direction == "sendonly";
}

}  // namespace

bool sdp_contains_media_section(const std::string& sdp) {
  const std::vector<std::string> lines = split_sdp_lines(sdp);
  return std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
    return starts_with(line, "m=");
  });
}

bool sdp_is_end_of_candidates_marker(const std::string& value) {
  const std::string trimmed = trim_copy(value);
  return trimmed == "a=end-of-candidates" || trimmed == "end-of-candidates";
}

std::string build_builtin_active_answer_sdp(const BuiltinSdpAnswerInput& input) {
  const std::vector<std::string> remote_lines = split_sdp_lines(input.remote_sdp);
  if (!sdp_contains_media_section(input.remote_sdp) || !input.dtls_identity_ready) {
    return input.fallback_local_description;
  }

  const std::vector<std::string> local_ice_lines =
    local_ice_lines_from_description(input.fallback_local_description);
  std::vector<std::string> out;
  out.push_back("v=0");
  out.push_back("o=- 0 0 IN IP4 127.0.0.1");
  out.push_back("s=-");
  out.push_back("t=0 0");
  for (const auto& line : remote_lines) {
    if (should_copy_session_attribute(line)) out.push_back(line);
  }

  bool in_media = false;
  std::vector<std::string> media_lines;
  auto flush_media = [&]() {
    if (media_lines.empty()) return;
    const bool media_is_audio = media_kind_from_mline(media_lines.front()) == "audio";
    out.push_back(rewrite_mline_for_ice_answer(media_lines.front()));
    out.push_back("c=IN IP4 0.0.0.0");
    for (size_t i = 1; i < media_lines.size(); ++i) {
      if (should_copy_media_attribute(media_lines[i])) out.push_back(media_lines[i]);
    }
    const std::string answer_direction = answer_direction_for_media_lines(media_lines);
    out.push_back("a=" + answer_direction);
    out.push_back("a=setup:" + input.dtls_setup_role);
    out.push_back("a=fingerprint:sha-256 " + input.dtls_fingerprint_sha256);
    for (const auto& line : local_ice_lines) out.push_back(line);
    if (media_is_audio && answer_direction_sends_media(answer_direction) &&
        input.outbound_audio_ssrc != 0) {
      const std::string stream_id =
        non_empty_or_default(input.outbound_audio_stream_id, "agentd_builtin_stream");
      const std::string track_id =
        non_empty_or_default(input.outbound_audio_track_id, "agentd_builtin_audio");
      const std::string cname =
        non_empty_or_default(input.outbound_audio_cname, "agentd-builtin-native");
      std::ostringstream ssrc;
      ssrc << input.outbound_audio_ssrc;
      out.push_back("a=msid:" + stream_id + " " + track_id);
      out.push_back("a=ssrc:" + ssrc.str() + " cname:" + cname);
      out.push_back("a=ssrc:" + ssrc.str() + " msid:" + stream_id + " " + track_id);
    }
    media_lines.clear();
  };

  for (const auto& line : remote_lines) {
    if (starts_with(line, "m=")) {
      if (in_media) flush_media();
      in_media = true;
      media_lines.push_back(line);
      continue;
    }
    if (in_media) media_lines.push_back(line);
  }
  flush_media();
  return join_sdp_lines(out);
}

}  // namespace agentd
