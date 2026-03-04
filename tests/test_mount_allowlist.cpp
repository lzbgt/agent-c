#include "mount_allowlist.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

using agentd::AllowedRoot;
using agentd::MountAllowlist;
using agentd::MountAllowlistDecision;
using agentd::MountAllowlistInput;
using agentd::mount_allowlist_validate;

namespace {

static std::string unique_suffix() {
#if defined(__unix__) || defined(__APPLE__)
  const long pid = (long)getpid();
#else
  const long pid = 0;
#endif
  return std::to_string(pid) + "_allowlist";
}

static std::filesystem::path make_temp_dir() {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  const std::filesystem::path dir = base / ("agentd_mount_allowlist_" + unique_suffix());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

static MountAllowlistDecision validate(
  const MountAllowlist* allow,
  const std::string& host_path,
  const std::string& container_path,
  bool is_main
) {
  MountAllowlistInput input;
  input.host_path = host_path;
  input.container_path = container_path;
  input.container_prefix = "/workspace/extra";
  input.is_main = is_main;
  return mount_allowlist_validate(allow, input);
}

}  // namespace

int main() {
  const std::filesystem::path tmp = make_temp_dir();
  const std::filesystem::path root = tmp / "root";
  const std::filesystem::path ok = root / "ok";
  const std::filesystem::path blocked = root / ".ssh";
  const std::filesystem::path outside = tmp / "other";

  std::error_code ec;
  std::filesystem::create_directories(ok, ec);
  std::filesystem::create_directories(blocked, ec);
  std::filesystem::create_directories(outside, ec);

  MountAllowlist allow;
  AllowedRoot ar;
  ar.path = root.string();
  ar.readonly = false;
  allow.allowed_roots.push_back(ar);
  allow.blocked_patterns.push_back(".ssh");
  allow.non_main_readonly = true;

  {
    const auto res = validate(&allow, ok.string(), "/workspace/extra/ok", true);
    assert(res.allowed);
    assert(res.readonly == false);
    assert(res.reason == "ok");
  }

  {
    const auto res = validate(&allow, ok.string(), "/workspace/extra/ok", false);
    assert(res.allowed);
    assert(res.readonly == true);
  }

  {
    const auto res = validate(&allow, blocked.string(), "/workspace/extra/blocked", true);
    assert(!res.allowed);
    assert(res.reason == "blocked_pattern");
    assert(res.blocked_pattern == ".ssh");
  }

  {
    const auto res = validate(&allow, ok.string(), "/etc", true);
    assert(!res.allowed);
    assert(res.reason == "container_path_outside_prefix");
  }

  {
    const auto res = validate(&allow, outside.string(), "/workspace/extra/outside", true);
    assert(!res.allowed);
    assert(res.reason == "host_path_outside_roots");
  }

  {
    const std::filesystem::path outside_file = outside / "secret";
    std::filesystem::create_directories(outside_file.parent_path(), ec);
    std::ofstream out(outside_file);
    out << "secret";
    out.close();

    const std::filesystem::path link = root / "link";
    std::error_code lerr;
    std::filesystem::create_symlink(outside, link, lerr);
    if (!lerr) {
      const std::filesystem::path via_link = link / "secret";
      const auto res = validate(&allow, via_link.string(), "/workspace/extra/link/secret", true);
      assert(!res.allowed);
      assert(res.reason == "host_path_outside_roots");
    }
  }

  {
    const auto res = validate(nullptr, ok.string(), "/workspace/extra/ok", true);
    assert(!res.allowed);
    assert(res.reason == "allowlist_missing");
  }

  std::filesystem::remove_all(tmp, ec);
  return 0;
}
