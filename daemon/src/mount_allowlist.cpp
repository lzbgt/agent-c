#include "mount_allowlist.h"

#include "json_util.h"
#include "run_endpoints_internal.h"

#include <cstdlib>
#include <filesystem>
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

}  // namespace agentd
