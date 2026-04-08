#include "host_session_prompts.h"

#include "default_system_prompt.h"

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
    want_client_profile ? session_leading_system_has_substring(session, client_profile_marker) : true;
  const bool have_any_project_instructions_leading = session_leading_system_has_prefix(session, kProjectInstructionsPrefix);
  const bool have_project_instructions_leading =
    want_project_instructions ? session_leading_system_has_exact(session, project_instructions_prompt) : true;
  const bool stale_client_profile_leading = !want_client_profile && have_any_client_profile_leading;
  const bool stale_project_instructions_leading = !want_project_instructions && have_any_project_instructions_leading;

  if (have_host_leading &&
      have_host_profile_leading &&
      have_client_profile_leading &&
      have_project_instructions_leading &&
      !stale_client_profile_leading &&
      !stale_project_instructions_leading) {
    return false;
  }

  struct Msg {
    agent_role_t role;
    std::string content;
  };

  std::vector<Msg> msgs;
  msgs.reserve(agent_session_message_count(session));
  for (size_t i = 0; i < agent_session_message_count(session); i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    msgs.push_back(Msg{v.role, std::string(v.content ? v.content : "", v.content_len)});
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
    (void)agent_session_add_message(rebuilt, m.role, m.content.c_str());
  }

  agent_session_destroy(session);
  *session_io = rebuilt;
  return true;
}
