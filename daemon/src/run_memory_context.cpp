#include "run_memory_context.h"

#include "memory_index.h"
#include "memory_salience.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
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

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static std::string trim_ascii(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::string to_generic_string(const std::filesystem::path& p) {
  return p.generic_string();
}

struct MemorySearchCandidate {
  std::string abs_path;
  std::string rel_path;
  std::string tier;
};

static std::string memory_relpath(const std::filesystem::path& abs, const std::filesystem::path& root) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
  if (ec) rel = abs.lexically_relative(root);
  return to_generic_string(rel.lexically_normal());
}

static void add_memory_candidate(
  const std::filesystem::path& mem_root,
  const std::filesystem::path& abs,
  const std::string& tier,
  std::vector<MemorySearchCandidate>* out
) {
  if (!out) return;
  MemorySearchCandidate c;
  c.abs_path = to_generic_string(abs);
  c.rel_path = memory_relpath(abs, mem_root);
  c.tier = tier;
  out->push_back(std::move(c));
}

static std::vector<MemorySearchCandidate> list_candidate_memory_files(
  const std::filesystem::path& mem_root,
  const std::string& session_id,
  const MemoryContextPolicy& pol
) {
  std::vector<MemorySearchCandidate> out;
  if (mem_root.empty()) return out;
  std::error_code ec;

  if (pol.include_structured) {
    const std::filesystem::path structured = mem_root / "STRUCTURED.md";
    if (std::filesystem::exists(structured, ec) && std::filesystem::is_regular_file(structured, ec)) {
      add_memory_candidate(mem_root, structured, "structured", &out);
    }
  }
  if (pol.include_core) {
    const std::filesystem::path core = mem_root / "MEMORY.md";
    ec.clear();
    if (std::filesystem::exists(core, ec) && std::filesystem::is_regular_file(core, ec)) {
      add_memory_candidate(mem_root, core, "core", &out);
    }
  }

  if (pol.include_session && is_safe_filename_component_ascii(session_id)) {
    const std::filesystem::path sessionp = mem_root / "sessions" / (session_id + ".md");
    ec.clear();
    if (std::filesystem::exists(sessionp, ec) && std::filesystem::is_regular_file(sessionp, ec)) {
      add_memory_candidate(mem_root, sessionp, "session", &out);
    }
  }

  if (pol.include_daily) {
    const int days = std::max(0, std::min(pol.daily_days, 31));
    for (int i = 0; i < days; i++) {
      const std::string ymd = local_date_ymd_days_ago(i);
      const std::filesystem::path p = mem_root / (ymd + ".md");
      ec.clear();
      if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        add_memory_candidate(mem_root, p, "daily", &out);
      }
    }
  }
  return out;
}

}  // namespace

static bool build_memory_files_context_text(
  const std::filesystem::path& mem_root,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;

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

static bool build_memory_index_context_text(
  const std::filesystem::path& mem_root,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;

  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    return false;
  }

  const std::vector<MemorySearchCandidate> candidates = list_candidate_memory_files(mem_root, session_id, pol);
  if (candidates.empty()) return false;

  auto count_lines = [](const std::filesystem::path& p, int64_t* out_lines) -> bool {
    if (!out_lines) return false;
    *out_lines = 0;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
      (*out_lines)++;
      if (*out_lines > 1000000) break; // defensive cap
    }
    return true;
  };
  auto estimate_tokens = [](int64_t bytes) -> int64_t {
    if (bytes <= 0) return 0;
    return (bytes + 3) / 4;
  };

  struct Row {
    std::string tier;
    std::string rel_path;
    int64_t bytes = 0;
    int64_t lines = 0;
    int64_t tokens = 0;
  };
  std::vector<Row> rows;
  rows.reserve(candidates.size());

  for (const auto& c : candidates) {
    std::filesystem::path abs(c.abs_path);
    ec.clear();
    if (!std::filesystem::exists(abs, ec) || !std::filesystem::is_regular_file(abs, ec)) continue;
    int64_t bytes = 0;
    ec.clear();
    const auto size = std::filesystem::file_size(abs, ec);
    if (!ec) bytes = (int64_t)size;
    int64_t lines = 0;
    (void)count_lines(abs, &lines);
    Row r;
    r.tier = c.tier.empty() ? "other" : c.tier;
    r.rel_path = c.rel_path.empty() ? memory_relpath(abs, mem_root) : c.rel_path;
    r.bytes = bytes;
    r.lines = lines;
    r.tokens = estimate_tokens(bytes);
    rows.push_back(std::move(r));
  }

  if (rows.empty()) return false;

  std::ostringstream oss;
  oss
    << "DURABLE_MEMORY_INDEX\n"
    << "- This is a lightweight index of durable memory files on disk.\n"
    << "- Token estimates are approximate (bytes/4). Use memory_search and memory_get for details.\n"
    << "- Use memory_write/memory_put to update durable memory when facts change.\n";

  for (const auto& r : rows) {
    oss << "\n[" << r.tier << " " << r.rel_path << "]"
        << " lines=" << r.lines
        << " bytes=" << r.bytes
        << " ~tokens=" << r.tokens << "\n";
  }

  std::string s = oss.str();
  const size_t kTotalCap = pol.total_cap == 0 ? (size_t)12000 : std::min<size_t>(pol.total_cap, (size_t)40000);
  if (s.size() > kTotalCap) s.resize(kTotalCap);
  *out_text = std::move(s);
  return true;
}

static std::string format_score(double score) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(2) << score;
  return oss.str();
}

static bool build_memory_salience_context_text(
  const std::filesystem::path& mem_root,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;

  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    return false;
  }

  MemorySaliencePolicy sp;
  sp.include_structured = pol.include_structured;
  sp.include_daily = pol.include_daily;
  sp.daily_days = std::max(0, std::min(pol.daily_days, 31));
  sp.max_items = std::max(1, pol.salience_max_items);
  sp.max_structured_items = std::max(0, pol.salience_structured_max_items);
  sp.max_daily_items = std::max(0, pol.salience_daily_max_items);
  sp.half_life_days = pol.salience_half_life_days;
  sp.importance_weight = pol.salience_importance_weight;

  MemorySalienceReport rep;
  std::string err;
  if (!memory_salience_collect(mem_root, sp, &rep, &err)) {
    return false;
  }

  bool any = false;
  std::ostringstream oss;
  oss
    << "DURABLE_MEMORY_SALIENCE\n"
    << "- Ranked by recency + importance (deterministic).\n"
    << "- Use memory_query for structured keys and memory_timeline for daily citations.\n";

  if (!rep.structured_items.empty()) {
    any = true;
    oss << "\n[structured]\n";
    for (const auto& item : rep.structured_items) {
      oss << "- (score=" << format_score(item.score) << ") ";
      if (!item.kind.empty()) oss << item.kind << " ";
      if (!item.key.empty()) oss << item.key << " = ";
      oss << item.text;
      if (!item.status.empty()) oss << " (" << item.status << ")";
      if (!item.ts_utc.empty()) oss << " [updated " << item.ts_utc << "]";
      oss << "\n";
    }
  }

  if (!rep.daily_items.empty()) {
    any = true;
    oss << "\n[daily]\n";
    for (const auto& item : rep.daily_items) {
      oss << "- (score=" << format_score(item.score) << ") ";
      if (!item.path.empty()) oss << "[" << item.path << ":" << item.line << "] ";
      oss << item.text;
      if (item.importance >= 0) oss << " (importance=" << item.importance << ")";
      if (!item.ts_utc.empty()) oss << " [ts " << item.ts_utc << "]";
      oss << "\n";
    }
  }

  if (pol.include_core) {
    const std::filesystem::path core = mem_root / "MEMORY.md";
    ec.clear();
    if (std::filesystem::exists(core, ec) && std::filesystem::is_regular_file(core, ec)) {
      std::string content = read_file_capped(core, 1200);
      content = strip_agent_memory_v1_json_block(content);
      if (!trim_copy(content).empty()) {
        any = true;
        oss << "\n[core MEMORY.md]\n";
        oss << content;
        if (!content.empty() && content.back() != '\n') oss << "\n";
      }
    }
  }

  if (pol.include_session && is_safe_filename_component_ascii(session_id)) {
    const std::filesystem::path sessionp = mem_root / "sessions" / (session_id + ".md");
    ec.clear();
    if (std::filesystem::exists(sessionp, ec) && std::filesystem::is_regular_file(sessionp, ec)) {
      std::string content = read_file_capped(sessionp, 1200);
      if (!trim_copy(content).empty()) {
        any = true;
        oss << "\n[session " << session_id << "]\n";
        oss << content;
        if (!content.empty() && content.back() != '\n') oss << "\n";
      }
    }
  }

  if (!any) return false;
  std::string s = oss.str();
  const size_t kTotalCap = pol.total_cap == 0 ? (size_t)12000 : std::min<size_t>(pol.total_cap, (size_t)40000);
  if (s.size() > kTotalCap) s.resize(kTotalCap);
  *out_text = std::move(s);
  return true;
}

static bool build_memory_search_context_text(
  const std::filesystem::path& mem_root,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  const std::string& query_raw,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;
  if (query_raw.empty()) return false;

  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    return false;
  }

  std::string query = trim_ascii(query_raw);
  if (query.empty()) return false;
  if (query.size() > 2000) query.resize(2000);
  for (char& c : query) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }

  const std::vector<MemorySearchCandidate> candidates = list_candidate_memory_files(mem_root, session_id, pol);
  if (candidates.empty()) return false;

  std::vector<std::string> files;
  files.reserve(candidates.size());
  std::unordered_map<std::string, std::string> tier_by_rel;
  for (const auto& c : candidates) {
    files.push_back(c.abs_path);
    if (!c.rel_path.empty() && tier_by_rel.find(c.rel_path) == tier_by_rel.end()) {
      tier_by_rel[c.rel_path] = c.tier;
    }
  }

  const int max_results = std::min(std::max(1, pol.search_max_results), 200);
  const int max_snippet_chars = std::min(std::max(80, pol.search_max_snippet_chars), 4000);
  const int context_lines = std::min(std::max(0, pol.search_context_lines), 20);

  struct Result {
    std::string path;
    int line = 1;
    std::string snippet;
    std::string tier;
    double score = 0.0;
  };
  std::vector<Result> results;
  results.reserve((size_t)max_results);

  auto tier_for_rel = [&](const std::string& rel_path) -> std::string {
    auto it = tier_by_rel.find(rel_path);
    if (it != tier_by_rel.end()) return it->second;
    return "other";
  };

  std::string mode = "substr";
  if (pol.search_use_index && !pol.search_case_sensitive) {
    std::vector<host_tools_internal::MemorySearchHit> hits;
    std::string err;
    if (host_tools_internal::memory_index_search_ranked(mem_root, files, query, max_results, max_snippet_chars, &hits, &err)) {
      mode = "fts5";
      for (const auto& h : hits) {
        Result r;
        r.path = h.path;
        r.line = h.line;
        r.snippet = h.snippet;
        r.tier = tier_for_rel(h.path);
        r.score = h.score;
        results.push_back(std::move(r));
      }
    }
  }

  if (results.empty()) {
    const std::string q = pol.search_case_sensitive ? query : to_lower_ascii(query);
    for (const auto& c : candidates) {
      if ((int)results.size() >= max_results) break;
      std::filesystem::path abs(c.abs_path);
      std::ifstream in(abs, std::ios::binary);
      if (!in.is_open()) continue;
      std::stringstream ss;
      ss << in.rdbuf();
      std::string content = ss.str();
      if (content.size() > 1024 * 1024) {
        content.resize(1024 * 1024);
      }
      std::vector<std::string> lines;
      lines.reserve(4096);
      {
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          lines.push_back(std::move(line));
          if (lines.size() > 20000) break;
        }
      }
      for (size_t i = 0; i < lines.size(); i++) {
        if ((int)results.size() >= max_results) break;
        const std::string hay = pol.search_case_sensitive ? lines[i] : to_lower_ascii(lines[i]);
        if (hay.find(q) == std::string::npos) continue;

        const int line_no = (int)i + 1;
        const int lo = std::max(1, line_no - context_lines);
        const int hi = std::min((int)lines.size(), line_no + context_lines);
        std::ostringstream snip;
        for (int ln = lo; ln <= hi; ln++) {
          if (ln > lo) snip << "\n";
          snip << lines[(size_t)ln - 1];
        }
        std::string snippet = snip.str();
        if ((int)snippet.size() > max_snippet_chars) {
          snippet.resize((size_t)max_snippet_chars);
        }

        Result r;
        r.path = c.rel_path.empty() ? memory_relpath(abs, mem_root) : c.rel_path;
        r.line = line_no;
        r.snippet = snippet;
        r.tier = c.tier.empty() ? "other" : c.tier;
        results.push_back(std::move(r));
      }
    }
  }

  if (results.empty()) return false;

  std::ostringstream oss;
  oss
    << "DURABLE_MEMORY_SEARCH_CONTEXT\n"
    << "- Query: " << query << "\n"
    << "- Mode: " << mode << "\n"
    << "- Results: " << results.size() << "\n"
    << "- Each snippet is cited as [tier path:line].\n"
    << "- Use memory_get/memory_search for deeper inspection, or memory_write/memory_put to update.\n";

  for (const auto& r : results) {
    oss << "\n[" << (r.tier.empty() ? "other" : r.tier) << " " << r.path << ":" << r.line;
    if (r.score != 0.0) {
      oss << " score=" << r.score;
    }
    oss << "]\n";
    oss << r.snippet;
    if (!r.snippet.empty() && r.snippet.back() != '\n') oss << "\n";
  }

  std::string s = oss.str();
  const size_t kTotalCap = pol.total_cap == 0 ? (size_t)12000 : std::min<size_t>(pol.total_cap, (size_t)40000);
  if (s.size() > kTotalCap) s.resize(kTotalCap);
  *out_text = std::move(s);
  return true;
}

bool build_memory_context_text(
  const std::string& state_dir,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  const std::string& query,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;
  if (state_dir.empty()) return false;

  const std::filesystem::path mem_root = std::filesystem::path(state_dir) / "memory";
  if (pol.mode == MemoryContextMode::Index) {
    if (build_memory_index_context_text(mem_root, session_id, pol, out_text)) return true;
    return false;
  }
  if (pol.mode == MemoryContextMode::Search) {
    if (build_memory_search_context_text(mem_root, session_id, pol, query, out_text)) return true;
    if (!pol.search_fallback_to_files) return false;
    return build_memory_files_context_text(mem_root, session_id, pol, out_text);
  }
  if (pol.mode == MemoryContextMode::Salience) {
    if (build_memory_salience_context_text(mem_root, session_id, pol, out_text)) return true;
    return false;
  }
  return build_memory_files_context_text(mem_root, session_id, pol, out_text);
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
    if (role == AGENT_ROLE_SYSTEM && !content.empty()) {
      if (content.rfind("DURABLE_MEMORY_CONTEXT", 0) == 0) return;
      if (content.rfind("DURABLE_MEMORY_SEARCH_CONTEXT", 0) == 0) return;
      if (content.rfind("DURABLE_MEMORY_INDEX", 0) == 0) return;
      if (content.rfind("DURABLE_MEMORY_SALIENCE", 0) == 0) return;
    }
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
