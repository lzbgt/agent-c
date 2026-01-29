#include "summary_compaction.h"

#include <algorithm>
#include <sstream>

static size_t count_pinned_system_prefix(const agent_session_t* session) {
  const size_t n = agent_session_message_count(session);
  size_t pinned = 0;
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) break;
    if (v.role == AGENT_ROLE_SYSTEM) {
      const std::string content(v.content ? v.content : "", v.content_len);
      if (content.rfind(AGENT_SESSION_SUMMARY_PREFIX, 0) == 0) {
        break;
      }
      pinned += 1;
      continue;
    }
    break;
  }
  // Safety: pin at least the first message if it is system (matches core behavior).
  if (pinned == 0 && n > 0) {
    agent_message_view_t v0{};
    if (agent_session_get_message(session, 0, &v0) == AGENT_OK && v0.role == AGENT_ROLE_SYSTEM) {
      const std::string c0(v0.content ? v0.content : "", v0.content_len);
      if (c0.rfind(AGENT_SESSION_SUMMARY_PREFIX, 0) != 0) {
        pinned = 1;
      }
    }
  }
  return pinned;
}

static const char* role_name(agent_role_t r) {
  switch (r) {
    case AGENT_ROLE_SYSTEM: return "system";
    case AGENT_ROLE_USER: return "user";
    case AGENT_ROLE_ASSISTANT: return "assistant";
    case AGENT_ROLE_TOOL: return "tool";
    default: return "unknown";
  }
}

SummaryCompactionInput build_summary_compaction_input(
  const agent_session_t* session,
  size_t keep_last_messages,
  size_t max_excerpt_chars,
  size_t max_messages,
  size_t per_message_chars
) {
  SummaryCompactionInput out;
  if (!session) {
    return out;
  }

  const size_t n = agent_session_message_count(session);
  const size_t keep_last = keep_last_messages == 0 ? 16 : keep_last_messages;

  out.pinned_system_messages = count_pinned_system_prefix(session);
  out.kept_suffix_messages = std::min(keep_last, n);

  if (n == 0) {
    return out;
  }

  const size_t suffix_start = n > out.kept_suffix_messages ? (n - out.kept_suffix_messages) : 0;
  const size_t drop_start = out.pinned_system_messages;
  const size_t drop_end = suffix_start;

  if (drop_end <= drop_start) {
    // No dropped window (prefix/suffix overlap).
    return out;
  }

  out.dropped_messages = drop_end - drop_start;

  std::ostringstream oss;
  bool truncated = false;
  size_t included = 0;

  const size_t to_take = std::min(max_messages, out.dropped_messages);
  for (size_t i = 0; i < to_take; i++) {
    const size_t idx = drop_start + i;
    agent_message_view_t v{};
    if (agent_session_get_message(session, idx, &v) != AGENT_OK) {
      truncated = true;
      break;
    }

    std::string content(v.content ? v.content : "", v.content_len);
    if (per_message_chars > 0 && content.size() > per_message_chars) {
      content.resize(per_message_chars);
      content += "...(truncated)";
      truncated = true;
    }

    oss << "[" << role_name(v.role) << "]\n";
    oss << content << "\n\n";
    included++;

    if (max_excerpt_chars > 0 && (size_t)oss.tellp() >= max_excerpt_chars) {
      truncated = true;
      break;
    }
  }

  std::string s = oss.str();
  if (max_excerpt_chars > 0 && s.size() > max_excerpt_chars) {
    s.resize(max_excerpt_chars);
    truncated = true;
  }

  if (included < out.dropped_messages) {
    truncated = true;
  }

  out.excerpt = s;
  out.truncated = truncated;
  return out;
}
