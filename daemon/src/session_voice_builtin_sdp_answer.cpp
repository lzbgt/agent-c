#include "session_voice_builtin_sdp_answer.h"

#include "session_voice_sdp_candidate.h"

#include <algorithm>
#include <cctype>
#include <limits>
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

std::string upper_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::vector<std::string> split_space_tokens(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> out;
  std::string token;
  while (stream >> token) out.push_back(token);
  return out;
}

bool parse_int(const std::string& value, int* out) {
  if (!out) return false;
  try {
    const long long parsed = std::stoll(value);
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
      return false;
    }
    *out = static_cast<int>(parsed);
    return true;
  } catch (...) {
    return false;
  }
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
      out.push_back(normalize_sdp_candidate_line(line));
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

std::vector<int> mline_payload_types(const std::string& line) {
  std::vector<int> out;
  if (!starts_with(line, "m=")) return out;
  const std::vector<std::string> parts = split_space_tokens(line.substr(2));
  for (size_t i = 3; i < parts.size(); ++i) {
    int payload_type = -1;
    if (parse_int(parts[i], &payload_type) && payload_type >= 0 && payload_type <= 127) {
      out.push_back(payload_type);
    }
  }
  return out;
}

bool rtpmap_for_payload(
  const std::string& line,
  int payload_type,
  std::string* out_codec,
  int* out_rate,
  int* out_channels
) {
  if (!starts_with(line, "a=rtpmap:")) return false;
  const std::string rest = line.substr(std::string("a=rtpmap:").size());
  const size_t space = rest.find(' ');
  if (space == std::string::npos) return false;
  int parsed_payload_type = -1;
  if (!parse_int(trim_copy(rest.substr(0, space)), &parsed_payload_type) ||
      parsed_payload_type != payload_type) {
    return false;
  }

  const std::string encoding = trim_copy(rest.substr(space + 1));
  const size_t slash_1 = encoding.find('/');
  if (slash_1 == std::string::npos) return false;
  const size_t slash_2 = encoding.find('/', slash_1 + 1);
  if (out_codec) *out_codec = upper_copy(trim_copy(encoding.substr(0, slash_1)));
  if (out_rate) {
    const std::string rate_token = slash_2 == std::string::npos
      ? encoding.substr(slash_1 + 1)
      : encoding.substr(slash_1 + 1, slash_2 - slash_1 - 1);
    int rate = 0;
    *out_rate = parse_int(trim_copy(rate_token), &rate) ? rate : 0;
  }
  if (out_channels) {
    int channels = 1;
    if (slash_2 != std::string::npos) {
      int parsed_channels = 0;
      if (parse_int(trim_copy(encoding.substr(slash_2 + 1)), &parsed_channels) &&
          parsed_channels > 0) {
        channels = parsed_channels;
      }
    }
    *out_channels = channels;
  }
  return true;
}

bool audio_payload_type_supported_for_answer(
  const std::vector<std::string>& media_lines,
  int payload_type
) {
  std::string codec;
  int rate = 0;
  int channels = 1;
  if (payload_type == 0) {
    codec = "PCMU";
    rate = 8000;
    channels = 1;
  } else if (payload_type == 8) {
    codec = "PCMA";
    rate = 8000;
    channels = 1;
  }
  for (const auto& line : media_lines) {
    (void)rtpmap_for_payload(line, payload_type, &codec, &rate, &channels);
  }

  if ((codec == "PCMU" || codec == "PCMA") && rate == 8000 && channels == 1) {
    return true;
  }
#if defined(AGENTD_HAVE_OPUS)
  if (codec == "OPUS" && rate == 48000 && (channels == 1 || channels == 2)) {
    return true;
  }
#endif
  return false;
}

std::vector<int> answerable_audio_payload_types(
  const std::vector<std::string>& media_lines
) {
  if (media_lines.empty()) return {};
  std::vector<int> out;
  for (const int payload_type : mline_payload_types(media_lines.front())) {
    if (audio_payload_type_supported_for_answer(media_lines, payload_type)) {
      out.push_back(payload_type);
    }
  }
  return out;
}

bool vector_contains_int(const std::vector<int>& values, int needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

bool media_attribute_payload_type(const std::string& line, int* out_payload_type) {
  const char* prefixes[] = {"a=rtpmap:", "a=fmtp:", "a=rtcp-fb:"};
  for (const char* prefix : prefixes) {
    if (!starts_with(line, prefix)) continue;
    const std::string rest = line.substr(std::string(prefix).size());
    const size_t end = rest.find_first_of(" \t");
    const std::string token = trim_copy(end == std::string::npos ? rest : rest.substr(0, end));
    if (token == "*") return false;
    int payload_type = -1;
    if (!parse_int(token, &payload_type)) return false;
    if (out_payload_type) *out_payload_type = payload_type;
    return true;
  }
  return false;
}

std::string rewrite_mline_for_ice_answer(
  const std::string& line,
  const std::vector<int>& audio_payload_types,
  bool reject_media
) {
  if (!starts_with(line, "m=")) return line;
  std::vector<std::string> parts = split_space_tokens(line.substr(2));
  if (parts.size() >= 2) parts[1] = reject_media ? "0" : "9";
  if (!reject_media && !parts.empty() && upper_copy(parts[0]) == "AUDIO" &&
      parts.size() > 3 && !audio_payload_types.empty()) {
    std::vector<std::string> filtered;
    filtered.insert(filtered.end(), parts.begin(), parts.begin() + 3);
    for (size_t i = 3; i < parts.size(); ++i) {
      int payload_type = -1;
      if (parse_int(parts[i], &payload_type) &&
          vector_contains_int(audio_payload_types, payload_type)) {
        filtered.push_back(parts[i]);
      }
    }
    parts = std::move(filtered);
  }
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

bool should_copy_media_attribute(
  const std::string& line,
  bool media_is_audio,
  const std::vector<int>& audio_payload_types
) {
  const bool recognized =
         starts_with(line, "a=mid:") ||
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
  if (!recognized) return false;
  if (!media_is_audio) return true;
  int payload_type = -1;
  if (!media_attribute_payload_type(line, &payload_type)) return true;
  return vector_contains_int(audio_payload_types, payload_type);
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
  return sdp_candidate_is_end_marker(value);
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
    const std::vector<int> audio_payload_types = media_is_audio
      ? answerable_audio_payload_types(media_lines)
      : std::vector<int>();
    const bool reject_media = media_is_audio && audio_payload_types.empty();
    out.push_back(rewrite_mline_for_ice_answer(
      media_lines.front(),
      audio_payload_types,
      reject_media));
    out.push_back("c=IN IP4 0.0.0.0");
    for (size_t i = 1; i < media_lines.size(); ++i) {
      if (should_copy_media_attribute(media_lines[i], media_is_audio, audio_payload_types)) {
        out.push_back(media_lines[i]);
      }
    }
    const std::string answer_direction = reject_media
      ? std::string("inactive")
      : answer_direction_for_media_lines(media_lines);
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
