#include "secrets_file.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace agentd {
namespace {

static std::string trim_ws(std::string s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
  s.erase(0, i);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}

static bool parse_env_line(const std::string& line, std::string* out_key, std::string* out_value) {
  if (!out_key || !out_value) return false;
  out_key->clear();
  out_value->clear();
  std::string s = trim_ws(line);
  if (s.empty() || s[0] == '#') return false;
  if (s.rfind("export ", 0) == 0) s = trim_ws(s.substr(std::strlen("export ")));
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return false;
  std::string k = trim_ws(s.substr(0, eq));
  std::string v = trim_ws(s.substr(eq + 1));
  if (k.empty()) return false;
  if (!v.empty() && v[0] != '"' && v[0] != '\'') {
    const size_t hash = v.find(" #");
    if (hash != std::string::npos) v = trim_ws(v.substr(0, hash));
  }
  if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
    v = v.substr(1, v.size() - 2);
  }
  if (v.empty()) return false;
  *out_key = k;
  *out_value = v;
  return true;
}

static std::string home_dir_best_effort() {
  if (const char* h = std::getenv("HOME")) {
    if (h[0]) return h;
  }
#if !defined(_WIN32)
  if (struct passwd* pw = getpwuid(getuid())) {
    if (pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
  }
#endif
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec && !cwd.empty()) return cwd.string();
  return std::string();
}

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

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_match(line, m, yaml_re) && m.size() >= 2) {
      const std::string k = m[1].str();
      if (looks_like_key(k)) return k;
    }
    std::string env_k;
    std::string env_v;
    if (parse_env_line(line, &env_k, &env_v)) {
      for (const auto& ev : env_vars) {
        if (env_k == ev && looks_like_key(env_v)) return env_v;
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
  const std::string home = home_dir_best_effort();
  if (!home.empty()) {
    out.push_back(std::filesystem::path(home) / ".env");
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
      const std::string home = home_dir_best_effort();
      if (!home.empty()) {
        const std::filesystem::path hp = std::filesystem::path(home) / ".env";
        if (hp == p) out.source_label = "~/.env";
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
