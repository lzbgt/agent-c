#include "sandbox_policy.h"

#include <filesystem>
#include <system_error>

namespace agentd {
namespace {

static std::filesystem::path canonicalize_best_effort(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
  if (!ec) {
    return canon;
  }
  return p.lexically_normal();
}

static bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& p) {
  auto it_r = root.begin();
  auto it_p = p.begin();
  for (; it_r != root.end(); ++it_r, ++it_p) {
    if (it_p == p.end()) return false;
    if (*it_r != *it_p) return false;
  }
  return true;
}

static std::string expand_special_root(const std::string& s, const std::string& host_scope_root) {
  if (s == "@host") return host_scope_root;
  if (s == "@cwd") return "";
  return s;
}

} // namespace

const char* host_policy_to_string(HostToolsetPolicyMode p) {
  return (p == HostToolsetPolicyMode::ReadOnly) ? "readonly" : "full";
}

bool host_policy_from_string(const std::string& s, HostToolsetPolicyMode* out) {
  if (!out) return false;
  if (s == "full") {
    *out = HostToolsetPolicyMode::Full;
    return true;
  }
  if (s == "readonly") {
    *out = HostToolsetPolicyMode::ReadOnly;
    return true;
  }
  return false;
}

HostToolsetPolicyMode tighten_host_policy(HostToolsetPolicyMode base, HostToolsetPolicyMode requested) {
  if (base == HostToolsetPolicyMode::ReadOnly) {
    return HostToolsetPolicyMode::ReadOnly;
  }
  if (requested == HostToolsetPolicyMode::ReadOnly) {
    return HostToolsetPolicyMode::ReadOnly;
  }
  return HostToolsetPolicyMode::Full;
}

bool sandbox_tighten_yolo(bool daemon_yolo_default, bool requested_yolo, bool requested_yolo_set) {
  const bool req = requested_yolo_set ? requested_yolo : daemon_yolo_default;
  return daemon_yolo_default && req;
}

bool sandbox_resolve_tools_root(
  const std::string& host_scope_root,
  bool effective_yolo,
  const std::string& daemon_tools_root,
  const std::string& requested_tools_root,
  bool requested_tools_root_set,
  std::string* out_effective_root,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_effective_root) {
    if (out_error) *out_error = "missing out_effective_root";
    return false;
  }

  if (effective_yolo) {
    *out_effective_root = "";
    return true;
  }

  const std::filesystem::path scope_root = canonicalize_best_effort(std::filesystem::path(host_scope_root));
  if (scope_root.empty()) {
    if (out_error) *out_error = "host_scope_root is empty";
    return false;
  }

  std::string base = expand_special_root(daemon_tools_root, host_scope_root);
  if (base.empty()) {
    base = host_scope_root;
  }

  std::string chosen = base;
  if (requested_tools_root_set) {
    chosen = expand_special_root(requested_tools_root, host_scope_root);
    if (chosen.empty()) {
      // Requests cannot loosen to unrestricted root when yolo is disabled; clamp to daemon default.
      chosen = base;
    }
  }

  std::filesystem::path root_path(chosen);
  if (root_path.is_relative()) {
    root_path = std::filesystem::path(host_scope_root) / root_path;
  }
  root_path = canonicalize_best_effort(root_path);

  if (!path_is_within(scope_root, root_path)) {
    if (out_error) *out_error = "tools_root escapes host_scope_root";
    return false;
  }

  *out_effective_root = root_path.string();
  return true;
}

}  // namespace agentd
