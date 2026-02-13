#include "secrets_file.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <cstdlib>

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

  std::vector<std::string> env_vars;
  if (provider == "deepseek") env_vars = {"DEEPSEEK_API_KEY"};
  else if (provider == "openrouter") env_vars = {"OPENROUTER_API_KEY"};
  else if (provider == "moonshot") env_vars = {"KIMI_API_KEY_CN", "MOONSHOT_API_KEY", "MOONSHOT_API_KEY_CN"};
  else if (provider == "openai") env_vars = {"OPENAI_API_KEY"};
  else return std::nullopt;

  const std::regex yaml_re("^\\s*-\\s*" + provider + "\\s*:\\s*(sk-[A-Za-z0-9_.-]+)\\s*$");
  std::vector<std::regex> env_res;
  env_res.reserve(env_vars.size());
  for (const auto& ev : env_vars) {
    env_res.emplace_back("^\\s*" + ev + "\\s*=\\s*(sk-[A-Za-z0-9_.-]+)\\s*$");
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_match(line, m, yaml_re) && m.size() >= 2) {
      const std::string k = m[1].str();
      if (looks_like_key(k)) return k;
    }
    for (const auto& re : env_res) {
      if (std::regex_match(line, m, re) && m.size() >= 2) {
        const std::string k = m[1].str();
        if (looks_like_key(k)) return k;
      }
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

  // Developer convenience: allow a user-level env file without requiring `export`.
  // This is intentionally a late fallback so project-local secrets override it.
  if (const char* h = std::getenv("HOME")) {
    if (h[0]) out.push_back(std::filesystem::path(h) / ".env");
  }
  return out;
}

}  // namespace

std::optional<std::string> load_provider_key_best_effort(const std::string& provider) {
  auto info = load_provider_key_source_best_effort(provider);
  if (info) return info->key;
  return std::nullopt;
}

std::optional<ProviderKeySource> load_provider_key_source_best_effort(const std::string& provider) {
  for (const auto& p : candidate_secret_files_best_effort()) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) continue;
    auto k = extract_key_from_file(p, provider);
    if (!k) continue;

    ProviderKeySource out;
    out.key = *k;
    out.source_kind = "file";

    const std::string name = p.filename().string();
    if (name == ".env") {
      if (const char* h = std::getenv("HOME")) {
        if (h[0]) {
          const std::filesystem::path hp = std::filesystem::path(h) / ".env";
          if (hp == p) out.source_label = "~/.env";
        }
      }
    }
    if (out.source_label.empty()) {
      if (name == ".not_in_repo") out.source_label = ".not_in_repo";
      else if (name == "project.local.md") out.source_label = "project.local.md";
      else out.source_label = name;
    }
    return out;
  }
  return std::nullopt;
}

}  // namespace agentd
