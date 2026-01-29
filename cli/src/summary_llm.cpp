#include "summary_llm.h"

#include "agent/agent.h"

#include <algorithm>
#include <cctype>
#include <vector>

static std::string trim(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && is_space((unsigned char)s.back())) s.pop_back();
  return s;
}

CompactionSummaryResult generate_compaction_summary_via_llm(
  const OpenAIClientConfig& base_cfg,
  const std::string& summary_model,
  const SummaryCompactionInput& input,
  size_t max_summary_chars
) {
  CompactionSummaryResult out;
  if (summary_model.empty()) {
    out.error = "missing summary_model";
    return out;
  }
  if (input.dropped_messages == 0 || input.excerpt.empty()) {
    out.error = "no dropped region";
    return out;
  }

  OpenAIClientConfig cfg = base_cfg;
  cfg.model = summary_model;

  const std::string system =
    "You are a summarization assistant.\n"
    "Summarize the dropped conversation context for continued work.\n"
    "Focus on: decisions, requirements, TODOs, file paths/symbols, commands, and any incomplete work.\n"
    "Be concise and structured. Avoid filler.\n"
    "Do not invent facts.\n";

  std::string user;
  user += "Dropped region excerpt (may be truncated):\n\n";
  user += input.excerpt;
  user += "\n\n";
  user += "Return a compact summary suitable to insert as a single system message.\n";

  agent_message_view_t msgs[2]{};
  msgs[0].role = AGENT_ROLE_SYSTEM;
  msgs[0].content = system.c_str();
  msgs[0].content_len = system.size();
  msgs[1].role = AGENT_ROLE_USER;
  msgs[1].content = user.c_str();
  msgs[1].content_len = user.size();

  const OpenAIChatResult r = openai_chat_completions(cfg, msgs, 2);
  out.http_status = r.http_status;
  out.http_body = r.response_body;
  if (r.http_status < 200 || r.http_status >= 300) {
    out.ok = false;
    out.error = !r.error_message.empty() ? r.error_message : openai_format_http_error(r.http_status, r.response_body);
    return out;
  }
  std::string s = trim(r.assistant_text);
  if (s.empty()) {
    out.ok = false;
    out.error = "summary model returned empty text";
    return out;
  }
  if (max_summary_chars > 0 && s.size() > max_summary_chars) {
    s.resize(max_summary_chars);
    s = trim(s);
  }
  out.ok = true;
  out.summary_text = s;
  return out;
}

