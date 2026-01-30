#include "secrets_file.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace agentd {
namespace {

static bool looks_like_key(const std::string& s) {
  if (s.size() < 6) return false;
  if (s.rfind("sk-", 0) != 0) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static std::optional<std::string> extract_key_from_file(const std::filesystem::path& file, const std::string& provider) {
  std::ifstream in(file);
  if (!in.is_open()) return std::nullopt;

  std::string env_var;
  if (provider == "deepseek") env_var = "DEEPSEEK_API_KEY";
  else if (provider == "openrouter") env_var = "OPENROUTER_API_KEY";
  else if (provider == "openai") env_var = "OPENAI_API_KEY";
  else return std::nullopt;

  const std::regex yaml_re("^\\s*-\\s*" + provider + "\\s*:\\s*(sk-[A-Za-z0-9_.-]+)\\s*$");
  const std::regex env_re("^\\s*" + env_var + "\\s*=\\s*(sk-[A-Za-z0-9_.-]+)\\s*$");

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_match(line, m, yaml_re) && m.size() >= 2) {
      const std::string k = m[1].str();
      if (looks_like_key(k)) return k;
    }
    if (std::regex_match(line, m, env_re) && m.size() >= 2) {
      const std::string k = m[1].str();
      if (looks_like_key(k)) return k;
    }
  }
  return std::nullopt;
}

static std::vector<std::filesystem::path> candidate_secret_files_best_effort() {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  std::filesystem::path cur = std::filesystem::current_path(ec);
  if (ec) cur = std::filesystem::path(".");
  cur = cur.lexically_normal();

  for (int depth = 0; depth < 8; depth++) {
    out.push_back(cur / ".not_in_repo");
    out.push_back(cur / "project.local.md");
    const auto parent = cur.parent_path();
    if (parent == cur || parent.empty()) break;
    cur = parent;
  }
  return out;
}

}  // namespace

std::optional<std::string> load_provider_key_best_effort(const std::string& provider) {
  for (const auto& p : candidate_secret_files_best_effort()) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) continue;
    auto k = extract_key_from_file(p, provider);
    if (k) return k;
  }
  return std::nullopt;
}

}  // namespace agentd

