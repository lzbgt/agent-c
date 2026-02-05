#include "run_memory_context.h"

#include "string_util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {
namespace {

static bool is_safe_filename_component_ascii(const std::string& s) {
  if (s.empty() || s.size() > 160) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string local_date_ymd_days_ago(int days_ago) {
  const auto now = std::chrono::system_clock::now() - std::chrono::hours(24 * std::max(0, days_ago));
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

static std::string read_file_capped(const std::filesystem::path& p, size_t max_bytes) {
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  if (s.size() > max_bytes) s.resize(max_bytes);
  return s;
}

static std::string strip_agent_memory_v1_json_block(const std::string& s) {
  // Remove the structured JSON blob (which is primarily machine metadata) to keep the injected
  // memory context human-readable.
  const std::string begin = "<!-- AGENT_MEMORY_V1_BEGIN -->";
  const std::string end = "<!-- AGENT_MEMORY_V1_END -->";
  const size_t a = s.find(begin);
  if (a == std::string::npos) return s;
  const size_t b = s.find(end, a + begin.size());
  if (b == std::string::npos) return s;
  const size_t end_b = b + end.size();
  std::string out;
  out.reserve(s.size());
  out.append(s.data(), a);
  out += "<!-- (structured memory metadata omitted) -->\n";
  if (end_b < s.size()) {
    out.append(s.data() + end_b, s.size() - end_b);
  }
  return out;
}

}  // namespace

bool build_memory_context_text(
  const std::string& state_dir,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;
  if (state_dir.empty()) return false;

  const std::filesystem::path mem_root = std::filesystem::path(state_dir) / "memory";

  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    return false;
  }

  struct Candidate {
    std::string label;
    std::filesystem::path path;
    size_t cap;
  };
  std::vector<Candidate> cands;
  if (pol.include_structured) cands.push_back(Candidate{"STRUCTURED.md", mem_root / "STRUCTURED.md", 6000});
  if (pol.include_core) cands.push_back(Candidate{"MEMORY.md", mem_root / "MEMORY.md", 6000});
  if (pol.include_daily) {
    const int days = std::max(0, std::min(pol.daily_days, 31));
    for (int i = 0; i < days; i++) {
      const std::string ymd = local_date_ymd_days_ago(i);
      cands.push_back(Candidate{ymd + ".md", mem_root / (ymd + ".md"), 2200});
    }
  }
  if (pol.include_session && is_safe_filename_component_ascii(session_id)) {
    cands.push_back(Candidate{"sessions/" + session_id + ".md", mem_root / "sessions" / (session_id + ".md"), 2400});
  }

  std::ostringstream oss;
  oss
    << "DURABLE_MEMORY_CONTEXT\n"
    << "- The following notes are loaded from durable Markdown memory files on disk.\n"
    << "- Treat these as authoritative *if still current*.\n"
    << "- If you discover a requirement has changed (\"used to be true\" -> \"no longer true\"), update memory to remove contradictions.\n"
    << "- Use memory_get/memory_search to inspect and memory_write/memory_put to persist/consolidate.\n";

  int included = 0;
  for (const auto& c : cands) {
    ec.clear();
    if (!std::filesystem::exists(c.path, ec) || !std::filesystem::is_regular_file(c.path, ec)) continue;
    std::string content = read_file_capped(c.path, c.cap);
    content = strip_agent_memory_v1_json_block(content);
    if (trim_copy(content).empty()) continue;
    included++;
    oss << "\n[" << c.label << "]\n";
    oss << content;
    if (!content.empty() && content.back() != '\n') oss << "\n";
  }
  if (included == 0) return false;

  std::string s = oss.str();
  const size_t kTotalCap = pol.total_cap == 0 ? (size_t)12000 : std::min<size_t>(pol.total_cap, (size_t)40000);
  if (s.size() > kTotalCap) s.resize(kTotalCap);
  *out_text = std::move(s);
  return true;
}

agent_session_t* clone_session_with_memory_context(const agent_session_t* src, const std::string& memory_context) {
  if (!src) return nullptr;
  if (memory_context.empty()) return nullptr;

  agent_session_t* ns = nullptr;
  if (agent_session_create(&ns) != AGENT_OK || !ns) return nullptr;

  const size_t n = agent_session_message_count(src);
  size_t pinned_system = 0;
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM) break;
    pinned_system++;
  }

  auto add_msg = [&](agent_role_t role, const agent_message_view_t& v) {
    const std::string content(v.content ? v.content : "", v.content_len);
    if (role == AGENT_ROLE_SYSTEM && !content.empty() && content.rfind("DURABLE_MEMORY_CONTEXT", 0) == 0) return;
    (void)agent_session_add_message(ns, role, content.c_str());
  };

  for (size_t i = 0; i < pinned_system; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    add_msg(v.role, v);
  }

  (void)agent_session_add_message(ns, AGENT_ROLE_SYSTEM, memory_context.c_str());

  for (size_t i = pinned_system; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    add_msg(v.role, v);
  }

  return ns;
}

}  // namespace agentd

