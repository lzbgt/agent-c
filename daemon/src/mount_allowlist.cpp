#include "mount_allowlist.h"

#include "json_util.h"
#include "run_endpoints_internal.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>

namespace agentd {
namespace {

struct AllowlistCache {
  bool loaded = false;
  bool attempted = false;
  MountAllowlist allowlist;
  MountAllowlistStatus status;
};

static std::vector<std::string> default_blocked_patterns() {
  return {
    ".ssh",
    ".gnupg",
    ".aws",
    ".kube",
    ".docker",
    ".env",
    "id_rsa",
    "id_ed25519",
    "private_key",
  };
}

static AllowlistCache& cache() {
  static AllowlistCache c;
  return c;
}

static bool parse_allowlist_json(const std::string& content, MountAllowlist* out, std::string* out_err) {
  if (!out) return false;
  Json::Value root;
  if (!json_parse_object(content, &root, out_err)) return false;

  if (!root.isMember("allowed_roots") || !root["allowed_roots"].isArray()) {
    if (out_err) *out_err = "allowed_roots must be an array";
    return false;
  }

  MountAllowlist parsed;
  for (const auto& entry : root["allowed_roots"]) {
    if (!entry.isObject()) {
      if (out_err) *out_err = "allowed_roots entries must be objects";
      return false;
    }
    const auto& path = entry["path"];
    if (!path.isString() || path.asString().empty()) {
      if (out_err) *out_err = "allowed_roots.path must be a non-empty string";
      return false;
    }
    AllowedRoot r;
    r.path = path.asString();
    if (entry.isMember("readonly")) {
      if (!entry["readonly"].isBool()) {
        if (out_err) *out_err = "allowed_roots.readonly must be boolean";
        return false;
      }
      r.readonly = entry["readonly"].asBool();
    }
    parsed.allowed_roots.push_back(r);
  }

  if (root.isMember("blocked_patterns")) {
    if (!root["blocked_patterns"].isArray()) {
      if (out_err) *out_err = "blocked_patterns must be an array";
      return false;
    }
    for (const auto& entry : root["blocked_patterns"]) {
      if (!entry.isString()) {
        if (out_err) *out_err = "blocked_patterns entries must be strings";
        return false;
      }
      const std::string s = entry.asString();
      if (!s.empty()) parsed.blocked_patterns.push_back(s);
    }
  }

  if (root.isMember("non_main_readonly")) {
    if (!root["non_main_readonly"].isBool()) {
      if (out_err) *out_err = "non_main_readonly must be boolean";
      return false;
    }
    parsed.non_main_readonly = root["non_main_readonly"].asBool();
  }

  std::set<std::string> merged;
  for (const auto& s : default_blocked_patterns()) merged.insert(s);
  for (const auto& s : parsed.blocked_patterns) merged.insert(s);
  parsed.blocked_patterns.assign(merged.begin(), merged.end());

  *out = parsed;
  return true;
}

static void ensure_loaded() {
  auto& c = cache();
  if (c.attempted) return;
  c.attempted = true;
  c.status = MountAllowlistStatus{};

  const std::string path = mount_allowlist_path();
  c.status.path = path;

  if (path.empty()) {
    c.status.error = "HOME not set; allowlist path unavailable";
    c.loaded = false;
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    c.status.present = false;
    c.status.error = "allowlist not found";
    c.loaded = false;
    return;
  }

  c.status.present = true;

  std::string content;
  if (!read_file_bytes_capped(path, 512 * 1024, &content)) {
    c.status.error = "failed to read allowlist";
    c.loaded = false;
    return;
  }

  std::string parse_err;
  MountAllowlist parsed;
  if (!parse_allowlist_json(content, &parsed, &parse_err)) {
    c.status.error = parse_err.empty() ? "invalid allowlist" : parse_err;
    c.loaded = false;
    return;
  }

  c.allowlist = parsed;
  c.loaded = true;
  c.status.loaded = true;
  c.status.allowed_roots = parsed.allowed_roots.size();
  c.status.blocked_patterns = parsed.blocked_patterns.size();
}

static std::filesystem::path expand_home_path(const std::string& raw) {
  if (raw.empty()) return std::filesystem::path();
  if (raw == "~" || raw.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return std::filesystem::path(raw);
    if (raw == "~") return std::filesystem::path(home);
    return std::filesystem::path(home) / raw.substr(2);
  }
  return std::filesystem::path(raw);
}

static std::optional<std::filesystem::path> canonical_existing(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return std::nullopt;
  std::filesystem::path c = std::filesystem::canonical(p, ec);
  if (ec) return std::nullopt;
  return c;
}

static bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  auto it_r = root.begin();
  auto it_c = candidate.begin();
  for (; it_r != root.end(); ++it_r, ++it_c) {
    if (it_c == candidate.end()) return false;
    if (*it_r != *it_c) return false;
  }
  return true;
}

static bool container_path_valid(const std::filesystem::path& p) {
  if (!p.is_absolute()) return false;
  for (const auto& comp : p) {
    if (comp == "..") return false;
  }
  return true;
}

static bool match_blocked_pattern(
  const std::filesystem::path& p,
  const std::vector<std::string>& patterns,
  std::string* out_pattern
) {
  if (out_pattern) out_pattern->clear();
  if (patterns.empty()) return false;
  const std::string full = p.generic_string();
  for (const auto& pat : patterns) {
    if (pat.empty()) continue;
    for (const auto& comp : p) {
      const std::string seg = comp.generic_string();
      if (seg == pat || seg.find(pat) != std::string::npos) {
        if (out_pattern) *out_pattern = pat;
        return true;
      }
    }
    if (full.find(pat) != std::string::npos) {
      if (out_pattern) *out_pattern = pat;
      return true;
    }
  }
  return false;
}

}  // namespace

std::string mount_allowlist_path() {
  const char* home = std::getenv("HOME");
  if (!home || !home[0]) return "";
  return std::string(home) + "/.config/agent/mount-allowlist.json";
}

const MountAllowlist* mount_allowlist_or_null() {
  ensure_loaded();
  auto& c = cache();
  if (!c.loaded) return nullptr;
  return &c.allowlist;
}

MountAllowlistStatus mount_allowlist_status() {
  ensure_loaded();
  return cache().status;
}

MountAllowlistDecision mount_allowlist_validate(const MountAllowlist* allowlist, const MountAllowlistInput& input) {
  MountAllowlistDecision out;
  if (!allowlist) {
    out.reason = "allowlist_missing";
    return out;
  }

  if (input.host_path.empty()) {
    out.reason = "host_path_missing";
    return out;
  }

  const std::filesystem::path host_raw = expand_home_path(input.host_path);
  const auto host_real_opt = canonical_existing(host_raw);
  if (!host_real_opt.has_value()) {
    out.reason = "host_path_unresolvable";
    return out;
  }
  const std::filesystem::path host_real = *host_real_opt;
  out.resolved_host_path = host_real.generic_string();

  const std::string prefix_raw = input.container_prefix.empty() ? "/workspace/extra" : input.container_prefix;
  const std::filesystem::path prefix = std::filesystem::path(prefix_raw).lexically_normal();
  if (prefix.empty() || !prefix.is_absolute()) {
    out.reason = "container_prefix_invalid";
    return out;
  }
  if (input.container_path.empty()) {
    out.reason = "container_path_missing";
    return out;
  }
  const std::filesystem::path container = std::filesystem::path(input.container_path).lexically_normal();
  if (!container_path_valid(container)) {
    out.reason = "container_path_invalid";
    return out;
  }
  if (!path_is_within(prefix, container)) {
    out.reason = "container_path_outside_prefix";
    return out;
  }
  out.resolved_container_path = container.generic_string();

  const AllowedRoot* matched = nullptr;
  std::filesystem::path matched_root;
  for (const auto& root : allowlist->allowed_roots) {
    if (root.path.empty()) continue;
    const std::filesystem::path root_raw = expand_home_path(root.path);
    const auto root_real_opt = canonical_existing(root_raw);
    if (!root_real_opt.has_value()) continue;
    const std::filesystem::path root_real = *root_real_opt;
    if (path_is_within(root_real, host_real)) {
      matched = &root;
      matched_root = root_real;
      break;
    }
  }
  if (!matched) {
    out.reason = "host_path_outside_roots";
    return out;
  }
  out.matched_root = matched_root.generic_string();

  std::string blocked;
  if (match_blocked_pattern(host_real, allowlist->blocked_patterns, &blocked)) {
    out.reason = "blocked_pattern";
    out.blocked_pattern = blocked;
    return out;
  }

  bool readonly = matched->readonly;
  if (!input.is_main && allowlist->non_main_readonly) {
    readonly = true;
  }

  out.allowed = true;
  out.readonly = readonly;
  out.reason = "ok";
  return out;
}

}  // namespace agentd
