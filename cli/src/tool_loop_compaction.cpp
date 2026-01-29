#include "tool_loop_compaction.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <algorithm>
#include <cctype>
#include <sstream>

static constexpr const char* kSummaryName = "__agent_compaction_summary__";

const char* tool_loop_compaction_summary_name() {
  return kSummaryName;
}

static std::string summarize_for_compaction_json(const Json::Value& msg, size_t snippet_chars) {
  std::string role = msg.isObject() && msg["role"].isString() ? msg["role"].asString() : "unknown";
  std::string content = msg.isObject() && msg["content"].isString() ? msg["content"].asString() : "";
  const size_t nl = content.find('\n');
  if (nl != std::string::npos) {
    content.resize(nl);
  }
  content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());

  auto ltrim = [&](std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    s.erase(0, i);
  };
  auto rtrim = [&](std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isspace((unsigned char)s[i - 1])) i--;
    s.resize(i);
  };
  ltrim(content);
  rtrim(content);

  if (snippet_chars > 0 && content.size() > snippet_chars) {
    content.resize(snippet_chars - 1);
    rtrim(content);
    content += "…";
  }
  if (content.empty()) return role;
  return role + ": " + content;
}

static std::string build_compaction_summary_json(
  const Json::Value& messages,
  size_t drop_begin,
  size_t drop_end, // exclusive
  const ToolLoopCompactionOptions& opt
) {
  if (!messages.isArray() || drop_end <= drop_begin) {
    return "";
  }
  const size_t dropped = (size_t)(drop_end - drop_begin);
  std::ostringstream oss;
  oss << "Previous conversation truncated (" << (unsigned long long)dropped
      << " earlier messages omitted) to stay within the model context window.\n";
  const size_t preview_n = std::min(opt.summary_preview_items, dropped);
  const size_t preview_start = drop_end - preview_n;
  for (size_t i = 0; i < preview_n; i++) {
    const Json::ArrayIndex idx = (Json::ArrayIndex)(preview_start + i);
    if (idx >= messages.size()) break;
    oss << (i + 1) << ". " << summarize_for_compaction_json(messages[idx], opt.summary_snippet_chars) << "\n";
  }
  std::string s = oss.str();
  if (!s.empty() && s.back() == '\n') s.pop_back();
  if (opt.summary_max_chars > 0 && s.size() > opt.summary_max_chars) {
    s.resize(opt.summary_max_chars - 1);
    s += "…";
  }
  return s;
}

size_t tool_loop_compaction_estimate_chars(const Json::Value& messages) {
  if (!messages.isArray()) {
    return 0;
  }
  size_t total = 0;
  for (Json::ArrayIndex i = 0; i < messages.size(); i++) {
    const auto& m = messages[i];
    if (!m.isObject()) continue;
    const auto& role = m["role"];
    const auto& content = m["content"];
    if (role.isString()) total += role.asString().size();
    if (content.isString()) total += content.asString().size();
    total += 8; // rough JSON overhead
  }
  return total;
}

size_t tool_loop_compaction_pinned_system_prefix_count(const Json::Value& messages) {
  if (!messages.isArray()) {
    return 0;
  }
  size_t pinned = 0;
  for (Json::ArrayIndex i = 0; i < messages.size(); i++) {
    const auto& m = messages[i];
    if (!m.isObject()) break;
    const auto& role = m["role"];
    if (!role.isString() || role.asString() != "system") break;
    const auto& name = m["name"];
    if (name.isString() && name.asString() == kSummaryName) {
      // Compaction summary is not "pinned".
      break;
    }
    pinned++;
  }
  return pinned;
}

bool tool_loop_compaction_maybe_compact_with_budget(
  Json::Value* messages,
  size_t max_chars_budget,
  const ToolLoopCompactionOptions& opt,
  ToolLoopCompactionReport* out_report
) {
  if (!messages || !messages->isArray()) {
    return false;
  }
  const size_t keep_last = opt.keep_last_messages == 0 ? 16 : opt.keep_last_messages;
  if (max_chars_budget == 0) {
    return false;
  }

  ToolLoopCompactionReport rep;
  rep.before_chars = tool_loop_compaction_estimate_chars(*messages);
  if (rep.before_chars <= max_chars_budget) {
    if (out_report) *out_report = rep;
    return false;
  }

  rep.pinned_system_messages = tool_loop_compaction_pinned_system_prefix_count(*messages);
  const size_t n = (size_t)messages->size();
  const size_t pinned = rep.pinned_system_messages;
  const size_t suffix_start = (n > keep_last) ? (n - keep_last) : pinned;
  const size_t drop_begin = pinned;
  const size_t drop_end = std::min(suffix_start, n);
  if (drop_end <= drop_begin) {
    if (out_report) *out_report = rep;
    return false;
  }

  rep.dropped_messages = drop_end - drop_begin;
  if (opt.insert_summary) {
    rep.summary = build_compaction_summary_json(*messages, drop_begin, drop_end, opt);
    rep.inserted_summary = !rep.summary.empty();
  }

  Json::Value out(Json::arrayValue);
  for (size_t i = 0; i < pinned; i++) {
    out.append((*messages)[(Json::ArrayIndex)i]);
  }
  if (rep.inserted_summary) {
    Json::Value m(Json::objectValue);
    m["role"] = "system";
    m["name"] = kSummaryName;
    m["content"] = rep.summary;
    out.append(m);
  }
  for (size_t i = drop_end; i < n; i++) {
    out.append((*messages)[(Json::ArrayIndex)i]);
  }

  *messages = out;
  rep.after_chars = tool_loop_compaction_estimate_chars(*messages);
  if (out_report) *out_report = rep;
  return true;
}

bool tool_loop_compaction_maybe_compact(
  Json::Value* messages,
  const ToolLoopCompactionOptions& opt,
  ToolLoopCompactionReport* out_report
) {
  const size_t max_chars = opt.max_chars == 0 ? 20000 : opt.max_chars;
  return tool_loop_compaction_maybe_compact_with_budget(messages, max_chars, opt, out_report);
}

#else
const char* tool_loop_compaction_summary_name() {
  return "__agent_compaction_summary__";
}
#endif

