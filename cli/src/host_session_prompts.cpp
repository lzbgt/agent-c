#include "host_session_prompts.h"

#include "default_system_prompt.h"
#include "agent/parts.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace {

constexpr size_t kMaxProjectInstructionFiles = 8;
constexpr size_t kMaxProjectInstructionFileBytes = 16 * 1024;
constexpr size_t kMaxProjectInstructionTotalBytes = 48 * 1024;
constexpr size_t kPinnedSystemScanLimit = 12;
constexpr const char* kHostPromptPrefix = "You are a host-side coding agent";
constexpr const char* kHostProfilePrefix = "HOST_SYSTEM_PROFILE=";
constexpr const char* kClientProfilePrefix = "CLIENT_PROFILE=";

struct ProjectInstructionDoc {
  std::string label;
  std::string content;
  bool truncated = false;
};

static std::filesystem::path normalize_scan_start(const std::filesystem::path& raw) {
  std::error_code ec;
  std::filesystem::path start = raw.empty() ? std::filesystem::current_path(ec) : raw;
  if (ec) return {};
  if (!start.is_absolute()) {
    const auto abs = std::filesystem::absolute(start, ec);
    if (!ec) start = abs;
  }
  const auto canon = std::filesystem::weakly_canonical(start, ec);
  if (!ec) start = canon;
  else start = start.lexically_normal();
  if (std::filesystem::is_regular_file(start, ec)) {
    start = start.parent_path();
  }
  return start;
}

static bool read_file_capped(const std::filesystem::path& path, size_t max_bytes, std::string* out, bool* truncated) {
  if (!out || !truncated) return false;
  *out = "";
  *truncated = false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string data;
  data.reserve(std::min<size_t>(max_bytes, 4096));
  char buf[4096];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    data.append(buf, static_cast<size_t>(got));
    if (data.size() > max_bytes) {
      *truncated = true;
      data.resize(max_bytes);
      break;
    }
  }
  if (!*truncated && !in.eof()) {
    *truncated = true;
  }
  if (data.find('\0') != std::string::npos) {
    return false;
  }
  *out = std::move(data);
  return true;
}

static std::vector<std::filesystem::path> discover_instruction_files(const std::filesystem::path& start_dir) {
  std::vector<std::filesystem::path> files;
  if (start_dir.empty()) return files;
  std::error_code ec;
  std::filesystem::path cur = start_dir;
  while (!cur.empty()) {
    const std::filesystem::path cand = cur / "AGENTS.md";
    if (std::filesystem::is_regular_file(cand, ec) && !ec) {
      files.push_back(cand.lexically_normal());
      if (files.size() >= kMaxProjectInstructionFiles) break;
    }
    const std::filesystem::path parent = cur.parent_path();
    if (parent == cur) break;
    cur = parent;
  }
  std::reverse(files.begin(), files.end());
  return files;
}

static std::vector<ProjectInstructionDoc> load_instruction_docs(const std::filesystem::path& start_dir) {
  std::vector<ProjectInstructionDoc> docs;
  const std::vector<std::filesystem::path> files = discover_instruction_files(start_dir);
  if (files.empty()) return docs;

  const std::filesystem::path label_root = files.front().parent_path();
  size_t total_bytes = 0;
  for (const auto& path : files) {
    if (total_bytes >= kMaxProjectInstructionTotalBytes) break;
    const size_t budget = std::min(kMaxProjectInstructionFileBytes, kMaxProjectInstructionTotalBytes - total_bytes);
    std::string content;
    bool truncated = false;
    if (!read_file_capped(path, budget, &content, &truncated)) continue;

    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(path, label_root, ec);
    const std::string label = (!ec && !rel.empty()) ? rel.generic_string() : path.filename().generic_string();

    total_bytes += content.size();
    docs.push_back(ProjectInstructionDoc{label, std::move(content), truncated});
  }
  return docs;
}


static bool copy_session_message(
    const agent_session_t* source, size_t index, agent_session_t* destination,
    agent_role_t role, const std::string& content) {
  const size_t count = agent_session_message_part_count(source, index);
  if (!count) return agent_session_add_message(destination, role, content.c_str()) == AGENT_OK;
  std::vector<agent_content_part_t> parts(count);
  for (size_t i = 0; i < count; ++i) {
    agent_content_part_view_t view{};
    if (agent_session_get_message_part(source, index, i, &view) != AGENT_OK) return false;
    parts[i] = {view.type, view.text, view.url, view.bytes, view.bytes_len, view.mime};
  }
  // Part views remain owned by source until core has copied them into destination.
  return agent_session_add_message_parts(destination, role, parts.data(), parts.size()) == AGENT_OK;
}

static bool session_leading_system_has_prefix(const agent_session_t* session, const char* prefix) {
  if (!session || !prefix || !prefix[0]) return false;
  const size_t prefix_len = std::strlen(prefix);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n && i < kPinnedSystemScanLimit; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) return false;
    if (v.content_len >= prefix_len && std::memcmp(v.content, prefix, prefix_len) == 0) return true;
  }
  return false;
}

static bool session_leading_system_has_substring(const agent_session_t* session, const std::string& needle) {
  if (!session || needle.empty()) return false;
  const size_t count = agent_session_message_count(session);
  for (size_t i = 0; i < count && i < kPinnedSystemScanLimit; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) return false;
    const std::string s(v.content, v.content_len);
    if (s.find(needle) != std::string::npos) return true;
  }
  return false;
}

static bool session_leading_system_has_exact(const agent_session_t* session, const std::string& want) {
  if (!session || want.empty()) return false;
  const size_t count = agent_session_message_count(session);
  for (size_t i = 0; i < count && i < kPinnedSystemScanLimit; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) return false;
    if (want.size() == v.content_len && std::memcmp(v.content, want.data(), want.size()) == 0) return true;
  }
  return false;
}

}  // namespace

std::string build_project_instructions_system_prompt(const std::filesystem::path& start_dir) {
  const std::filesystem::path scan_root = normalize_scan_start(start_dir);
  if (scan_root.empty()) return "";

  const std::vector<ProjectInstructionDoc> docs = load_instruction_docs(scan_root);
  if (docs.empty()) return "";

  std::ostringstream out;
  out << kProjectInstructionsPrefix << "\n\n";
  out << "The following AGENTS.md files were loaded from the working directory upward.\n";
  out << "They are ordered from parent directories to the current working directory.\n";
  out << "Later sections override earlier ones when they conflict.\n";

  for (const auto& doc : docs) {
    out << "\n[" << doc.label << "]\n";
    out << doc.content;
    if (!doc.content.empty() && doc.content.back() != '\n') out << "\n";
    if (doc.truncated) {
      out << "[truncated after " << doc.content.size() << " bytes to keep the prompt bounded]\n";
    }
  }
  return out.str();
}


std::string build_current_date_system_prompt(std::time_t now) {
  std::tm utc{}, local{};
  if (!gmtime_r(&now, &utc) || !localtime_r(&now, &local)) return "";
  char utc_date[16]{}, local_date[16]{}, offset[16]{}, zone[64]{};
  if (!std::strftime(utc_date, sizeof(utc_date), "%Y-%m-%d", &utc) ||
      !std::strftime(local_date, sizeof(local_date), "%Y-%m-%d", &local) ||
      !std::strftime(offset, sizeof(offset), "%z", &local)) return "";
  std::strftime(zone, sizeof(zone), "%Z", &local);
  std::ostringstream out;
  out << kCurrentDateContextPrefix << "\n"
      << "Host local date: " << local_date << " (UTC offset " << offset;
  if (*zone) out << ", zone " << zone;
  out << ").\n"
      << "Current UTC date: " << utc_date << ".\n"
      << "This runtime date supersedes date assumptions in model memory or earlier conversation. "
      << "Resolve today/latest relative to the host local date unless the user specifies a timezone or an explicit historical period. "
      << "For latest news, start web_search with time_range=day; broaden only if needed and disclose the actual coverage dates. "
      << "A newly fetched or modified page is not necessarily newly published. Verify publication and event dates in fetched sources; "
      << "undated pages and older reports are not proof of current news. Cite a supporting source URL and its publication/event date "
      << "for every news item, and state when a date is unknown. Do not invent dates or claim a future as-of date.\n";
  return out.str();
}

bool ensure_current_date_system_prompt(agent_session_t** session_io, std::time_t now) {
  if (!session_io || !*session_io) return false;
  const auto prompt = build_current_date_system_prompt(now);
  if (prompt.empty()) return false;
  agent_session_t* session = *session_io;
  struct Msg { agent_role_t role; std::string content; size_t source_index; };
  std::vector<Msg> messages;
  messages.reserve(agent_session_message_count(session));
  bool leading = true, have_current = false;
  size_t managed_count = 0;
  for (size_t i = 0; i < agent_session_message_count(session); ++i) {
    agent_message_view_t view{};
    if (agent_session_get_message(session, i, &view) != AGENT_OK) return false;
    const std::string content(view.content ? view.content : "", view.content_len);
    if (view.role != AGENT_ROLE_SYSTEM) leading = false;
    if (view.role == AGENT_ROLE_SYSTEM && content.rfind(kCurrentDateContextPrefix, 0) == 0) {
      ++managed_count;
      have_current = have_current || (leading && content == prompt);
      continue;
    }
    messages.push_back({view.role, content, i});
  }
  if (have_current && managed_count == 1) return false;
  agent_session_t* rebuilt = nullptr;
  if (agent_session_create(&rebuilt) != AGENT_OK || !rebuilt) return false;
  const auto append = [&](agent_role_t role, const std::string& content) {
    return agent_session_add_message(rebuilt, role, content.c_str()) == AGENT_OK;
  };
  bool added = false, ok = true;
  for (const auto& message : messages) {
    if (!added && message.role != AGENT_ROLE_SYSTEM) {
      ok = append(AGENT_ROLE_SYSTEM, prompt);
      added = true;
    }
    if (!ok || !(ok = copy_session_message(session, message.source_index, rebuilt, message.role, message.content))) break;
  }
  if (ok && !added) ok = append(AGENT_ROLE_SYSTEM, prompt);
  if (!ok) { agent_session_destroy(rebuilt); return false; }
  agent_session_destroy(session);
  *session_io = rebuilt;
  return true;
}

bool ensure_pinned_host_session_prompts(
  agent_session_t** session_io,
  const std::string& host_system_profile,
  const std::string& client_profile_prompt,
  const std::string& client_profile_marker,
  const std::string& project_instructions_prompt
) {
  if (!session_io || !*session_io) return false;

  agent_session_t* session = *session_io;
  const std::string host_prompt = host_system_prompt_for_profile(host_system_profile.empty() ? "default" : host_system_profile.c_str());
  const std::string host_profile_marker =
    std::string(kHostProfilePrefix) + (host_system_profile.empty() ? "default" : host_system_profile);
  const bool want_client_profile = !client_profile_prompt.empty() && !client_profile_marker.empty();
  const bool want_project_instructions = !project_instructions_prompt.empty();

  const bool have_host_leading = session_leading_system_has_prefix(session, kHostPromptPrefix);
  const bool have_host_profile_leading = session_leading_system_has_substring(session, host_profile_marker);
  const bool have_any_client_profile_leading = session_leading_system_has_prefix(session, kClientProfilePrefix);
  const bool have_client_profile_leading =
    want_client_profile ? session_leading_system_has_exact(session, client_profile_prompt) : true;
  const bool have_any_project_instructions_leading = session_leading_system_has_prefix(session, kProjectInstructionsPrefix);
  const bool have_project_instructions_leading =
    want_project_instructions ? session_leading_system_has_exact(session, project_instructions_prompt) : true;
  const bool stale_client_profile_leading = !want_client_profile && have_any_client_profile_leading;
  const bool stale_project_instructions_leading = !want_project_instructions && have_any_project_instructions_leading;

  if (have_host_leading &&
      have_host_profile_leading &&
      session_leading_system_has_exact(session, host_prompt) &&
      have_client_profile_leading &&
      have_project_instructions_leading &&
      !stale_client_profile_leading &&
      !stale_project_instructions_leading) {
    return false;
  }

  struct Msg {
    agent_role_t role;
    std::string content;
    size_t source_index;
  };

  std::vector<Msg> msgs;
  msgs.reserve(agent_session_message_count(session));
  for (size_t i = 0; i < agent_session_message_count(session); i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    msgs.push_back(Msg{v.role, std::string(v.content ? v.content : "", v.content_len), i});
  }

  agent_session_t* rebuilt = nullptr;
  if (agent_session_create(&rebuilt) != AGENT_OK || !rebuilt) {
    return false;
  }

  (void)agent_session_add_message(rebuilt, AGENT_ROLE_SYSTEM, host_prompt.c_str());
  if (want_client_profile) {
    (void)agent_session_add_message(rebuilt, AGENT_ROLE_SYSTEM, client_profile_prompt.c_str());
  }
  if (want_project_instructions) {
    (void)agent_session_add_message(rebuilt, AGENT_ROLE_SYSTEM, project_instructions_prompt.c_str());
  }

  for (const auto& m : msgs) {
    if (m.role == AGENT_ROLE_SYSTEM && !m.content.empty()) {
      if (m.content.rfind(kHostPromptPrefix, 0) == 0) continue;
      if (m.content.rfind(kHostProfilePrefix, 0) == 0) continue;
      if (m.content.rfind(kClientProfilePrefix, 0) == 0) continue;
      if (m.content.rfind(kProjectInstructionsPrefix, 0) == 0) continue;
    }
    if (!copy_session_message(session, m.source_index, rebuilt, m.role, m.content)) {
      agent_session_destroy(rebuilt);
      return false;
    }
  }

  agent_session_destroy(session);
  *session_io = rebuilt;
  return true;
}
