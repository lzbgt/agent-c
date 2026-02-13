#pragma once

#include <optional>
#include <string>

namespace agentd {

struct ProviderKeySource {
  std::string key;
  // e.g. "file"
  std::string source_kind;
  // e.g. ".not_in_repo", "project.local.md", "~/.env"
  std::string source_label;
};

// Best-effort local secrets discovery for daemon host use.
//
// This is intentionally simple and supports only the repo's gitignored secret file formats:
// - .not_in_repo (preferred)
// - project.local.md (fallback)
//
// Formats accepted (either file):
// - YAML-ish:
//   - deepseek: sk-...
//   - openrouter: sk-...
// - Env-ish:
//   DEEPSEEK_API_KEY=sk-...
//   OPENROUTER_API_KEY=sk-...
//
// This API is designed so the daemon can keep provider keys out of browser storage:
// the Web UI can omit api_key and rely on daemon-side key loading.
std::optional<std::string> load_provider_key_best_effort(const std::string& provider);
std::optional<ProviderKeySource> load_provider_key_source_best_effort(const std::string& provider);

}  // namespace agentd
