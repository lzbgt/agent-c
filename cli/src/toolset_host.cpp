#include "toolset_host.h"
#include "toolset_host_internal.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace host_tools_internal;

static std::string trim_ascii(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static bool str_contains(const std::string& s, char c) {
  return s.find(c) != std::string::npos;
}

static std::string normalize_gitignore_pattern(std::string p) {
  // Best-effort compatibility:
  // - treat ** like * (fnmatch doesn't implement git's ** semantics)
  // - keep pattern otherwise intact (including '?', '*', '[]')
  size_t pos = 0;
  while ((pos = p.find("**", pos)) != std::string::npos) {
    p.replace(pos, 2, "*");
    pos += 1;
  }
  return p;
}

static std::optional<std::filesystem::path> find_git_root_best_effort(std::filesystem::path start) {
  std::error_code ec;
  start = std::filesystem::weakly_canonical(start, ec);
  if (ec) start = start.lexically_normal();
  std::filesystem::path cur = start;
  for (int depth = 0; depth < 64; depth++) {
    if (cur.empty()) break;
    if (std::filesystem::exists(cur / ".git", ec) && std::filesystem::is_directory(cur / ".git", ec)) {
      return cur;
    }
    const std::filesystem::path parent = cur.parent_path();
    if (parent == cur) break;
    cur = parent;
  }
  return std::nullopt;
}

static std::string relpath_for_match(const std::filesystem::path& root, const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(p, root, ec);
  if (ec) {
    rel = p.lexically_relative(root);
  }
  return rel.generic_string();
}

static int64_t unix_ms_from_timespec(int64_t sec, int64_t nsec) {
  if (nsec < 0) nsec = 0;
  return sec * 1000 + nsec / 1000000;
}

static bool posix_stat_times_best_effort(
  const std::filesystem::path& p,
  int64_t* out_ctime_unix_ms,
  int64_t* out_birthtime_unix_ms
) {
  if (out_ctime_unix_ms) *out_ctime_unix_ms = 0;
  if (out_birthtime_unix_ms) *out_birthtime_unix_ms = 0;

  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) {
    return false;
  }

#if defined(__APPLE__)
  if (out_ctime_unix_ms) {
    *out_ctime_unix_ms = unix_ms_from_timespec((int64_t)st.st_ctimespec.tv_sec, (int64_t)st.st_ctimespec.tv_nsec);
  }
  if (out_birthtime_unix_ms) {
    *out_birthtime_unix_ms = unix_ms_from_timespec((int64_t)st.st_birthtimespec.tv_sec, (int64_t)st.st_birthtimespec.tv_nsec);
  }
#elif defined(__linux__)
  if (out_ctime_unix_ms) {
    *out_ctime_unix_ms = unix_ms_from_timespec((int64_t)st.st_ctim.tv_sec, (int64_t)st.st_ctim.tv_nsec);
  }
#else
  // Fallback: second-granularity ctime (inode change time on most POSIX systems).
  if (out_ctime_unix_ms) {
    *out_ctime_unix_ms = (int64_t)st.st_ctime * 1000;
  }
#endif
  return true;
}

static bool gitignore_rule_matches(
  const GitignoreRule& r,
  const std::string& relpath,
  const std::string& basename,
  bool is_dir
) {
  if (r.pattern.empty()) return false;
  const std::string& pat = r.pattern;

  auto fnm = [&](const std::string& candidate, const std::string& patt, int flags) -> bool {
    return ::fnmatch(patt.c_str(), candidate.c_str(), flags) == 0;
  };

  // If rule is "dir/" style, it should ignore the directory and everything under it.
  if (r.subtree) {
    if (r.has_slash) {
      const int flags = FNM_PATHNAME;
      if (r.anchored) {
        if (fnm(relpath, pat, flags)) return true;
        if (fnm(relpath, pat + "/*", flags)) return true;
        return false;
      }
      if (fnm(relpath, pat, flags)) return true;
      if (fnm(relpath, pat + "/*", flags)) return true;
      for (size_t i = 0; i < relpath.size(); i++) {
        if (relpath[i] != '/') continue;
        const std::string suffix = relpath.substr(i + 1);
        if (fnm(suffix, pat, flags)) return true;
        if (fnm(suffix, pat + "/*", flags)) return true;
      }
      return false;
    }

    // No-slash directory pattern like "build/": match any segment (directory itself or its descendants).
    if (basename.empty()) return false;
    if (is_dir && fnm(basename, pat, 0)) return true;
    size_t start = 0;
    for (;;) {
      const size_t slash = relpath.find('/', start);
      const std::string seg = (slash == std::string::npos) ? relpath.substr(start) : relpath.substr(start, slash - start);
      if (!seg.empty() && fnm(seg, pat, 0)) return true;
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    return false;
  }

  if (!r.has_slash) {
    return fnm(basename, pat, 0);
  }

  const int flags = FNM_PATHNAME;
  if (r.anchored) {
    return fnm(relpath, pat, flags);
  }
  if (fnm(relpath, pat, flags)) return true;
  for (size_t i = 0; i < relpath.size(); i++) {
    if (relpath[i] != '/') continue;
    const std::string suffix = relpath.substr(i + 1);
    if (fnm(suffix, pat, flags)) return true;
  }
  return false;
}

static bool load_gitignore_from_file(const std::filesystem::path& gitignore_path, std::vector<GitignoreRule>* out_rules) {
  if (!out_rules) return false;
  out_rules->clear();

  std::ifstream in(gitignore_path);
  if (!in.is_open()) return false;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = trim_ascii(line);
    if (line.empty()) continue;
    if (line[0] == '#') continue;

    // Handle escaped leading comment/negation markers.
    if (line.size() >= 2 && line[0] == '\\' && (line[1] == '#' || line[1] == '!')) {
      line = line.substr(1);
    }

    GitignoreRule r;
    if (!line.empty() && line[0] == '!') {
      r.negated = true;
      line = line.substr(1);
      line = trim_ascii(line);
      if (line.empty()) continue;
    }
    if (!line.empty() && line[0] == '/') {
      r.anchored = true;
      line = line.substr(1);
    }
    if (!line.empty() && line.back() == '/') {
      r.subtree = true;
      line.pop_back();
    }
    line = normalize_gitignore_pattern(line);
    if (line.empty()) continue;
    r.has_slash = str_contains(line, '/');
    r.pattern = line;
    out_rules->push_back(std::move(r));
  }
  return true;
}

static bool ensure_gitignore_cache(HostToolCtx* ctx) {
  if (!ctx) return false;

  // Prefer the actual git root if present; otherwise fall back to the tool root.
  std::filesystem::path root = ctx->root;
  if (auto gr = find_git_root_best_effort(ctx->root)) {
    root = *gr;
  }
  const std::filesystem::path gi = root / ".gitignore";

  std::error_code ec;
  if (!std::filesystem::exists(gi, ec) || !std::filesystem::is_regular_file(gi, ec)) {
    ctx->gitignore.ready = false;
    ctx->gitignore.rules.clear();
    ctx->gitignore.root = root;
    ctx->gitignore.file = gi;
    ctx->gitignore.mtime_unix_ms = 0;
    return false;
  }

  const auto mtime = std::filesystem::last_write_time(gi, ec);
  const int64_t mtime_ms = ec ? 0 : file_time_to_unix_ms(mtime);
  if (ctx->gitignore.ready && ctx->gitignore.file == gi && ctx->gitignore.mtime_unix_ms == mtime_ms) {
    return true;
  }

  std::vector<GitignoreRule> rules;
  if (!load_gitignore_from_file(gi, &rules)) {
    ctx->gitignore.ready = false;
    ctx->gitignore.rules.clear();
    ctx->gitignore.root = root;
    ctx->gitignore.file = gi;
    ctx->gitignore.mtime_unix_ms = mtime_ms;
    return false;
  }
  ctx->gitignore.ready = true;
  ctx->gitignore.root = root;
  ctx->gitignore.file = gi;
  ctx->gitignore.mtime_unix_ms = mtime_ms;
  ctx->gitignore.rules = std::move(rules);
  return true;
}

static bool gitignore_is_ignored(HostToolCtx* ctx, const std::filesystem::path& entry_path, bool is_dir) {
  if (!ctx) return false;
  if (!ctx->gitignore.ready) return false;

  std::string rel = relpath_for_match(ctx->gitignore.root, entry_path.lexically_normal());
  if (rel.empty() || rel == "." || rel == "..") return false;
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
  const std::string basename = entry_path.filename().string();

  bool ignored = false;
  for (const auto& r : ctx->gitignore.rules) {
    if (!gitignore_rule_matches(r, rel, basename, is_dir)) continue;
    ignored = !r.negated;
  }
  return ignored;
}

static bool glob_matches_any(const std::vector<std::string>& globs, const std::string& text) {
  if (globs.empty() || text.empty()) return false;
  for (const auto& g : globs) {
    if (g.empty()) continue;
    if (::fnmatch(g.c_str(), text.c_str(), 0) == 0) {
      return true;
    }
  }
  return false;
}

static bool is_probably_binary(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  char buf[4096];
  in.read(buf, sizeof(buf));
  const std::streamsize n = in.gcount();
  for (std::streamsize i = 0; i < n; i++) {
    if (buf[i] == '\0') return true;
  }
  return false;
}

static agent_status_t tool_fs_stat(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_stat requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }
  const bool count_lines = args.isMember("count_lines") && args["count_lines"].isBool() ? args["count_lines"].asBool() : false;
  const int max_count_bytes = args.isMember("max_count_bytes") && args["max_count_bytes"].isInt() ? args["max_count_bytes"].asInt() : (2 * 1024 * 1024);
  const int max_count_lines = args.isMember("max_count_lines") && args["max_count_lines"].isInt() ? args["max_count_lines"].asInt() : 200000;

  const std::string path = args["path"].asString();
  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  std::error_code ec;
  auto st = std::filesystem::status(*resolved, ec);
  if (ec) {
    return write_envelope(false, "stat failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["tool"] = "fs_stat";
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["root_dir"] = to_generic_string(ctx->root);
  data["unrestricted"] = ctx->unrestricted;
  data["exists"] = std::filesystem::exists(st);
  data["is_file"] = std::filesystem::is_regular_file(st);
  data["is_dir"] = std::filesystem::is_directory(st);
  if (data["exists"].asBool() && data["is_file"].asBool()) {
    data["size_bytes"] = (Json::UInt64)std::filesystem::file_size(*resolved, ec);
    data["is_binary"] = is_probably_binary(*resolved);
  }
  std::filesystem::file_time_type mtime{};
  ec.clear();
  mtime = std::filesystem::last_write_time(*resolved, ec);
  if (!ec) {
    data["mtime_unix_ms"] = (Json::Int64)file_time_to_unix_ms(mtime);
  }
  {
    int64_t ctime_ms = 0;
    int64_t birth_ms = 0;
    if (posix_stat_times_best_effort(*resolved, &ctime_ms, &birth_ms)) {
      if (ctime_ms > 0) data["ctime_unix_ms"] = (Json::Int64)ctime_ms;
      if (birth_ms > 0) data["birthtime_unix_ms"] = (Json::Int64)birth_ms;
    }
  }

  if (count_lines && data["exists"].asBool() && data["is_file"].asBool() && data.isMember("size_bytes")) {
    data["count_lines"] = true;
    data["max_count_bytes"] = max_count_bytes;
    data["max_count_lines"] = max_count_lines;

    const bool is_bin = data.isMember("is_binary") && data["is_binary"].isBool() && data["is_binary"].asBool();
    const uint64_t sz = data["size_bytes"].isUInt64() ? data["size_bytes"].asUInt64() : 0;

    if (is_bin) {
      data["line_count_available"] = false;
      data["line_count_reason"] = "binary_file";
    } else if (max_count_bytes > 0 && sz > (uint64_t)max_count_bytes) {
      data["line_count_available"] = false;
      data["line_count_reason"] = "file_too_large";
    } else if (max_count_lines < 1) {
      data["line_count_available"] = false;
      data["line_count_reason"] = "invalid_max_count_lines";
    } else {
      std::ifstream in(*resolved);
      if (!in.is_open()) {
        data["line_count_available"] = false;
        data["line_count_reason"] = "open_failed";
      } else {
        int64_t lines = 0;
        std::string line;
        while (std::getline(in, line)) {
          lines++;
          if (lines >= (int64_t)max_count_lines) {
            break;
          }
        }
        data["line_count_available"] = true;
        data["total_lines"] = (Json::Int64)lines;
        if (lines >= (int64_t)max_count_lines) {
          data["total_lines_truncated"] = true;
        }
      }
    }
  }
  {
    // Human-friendly output for UIs. Keep structured fields for the LLM.
    std::ostringstream oss;
    oss << "path: " << path << "\n";
    oss << "resolved_path: " << to_generic_string(*resolved) << "\n";
    oss << "exists: " << (data["exists"].asBool() ? "true" : "false") << "\n";
    oss << "is_file: " << (data["is_file"].asBool() ? "true" : "false") << "\n";
    oss << "is_dir: " << (data["is_dir"].asBool() ? "true" : "false") << "\n";
    if (data.isMember("size_bytes")) {
      oss << "size_bytes: " << (unsigned long long)data["size_bytes"].asUInt64() << "\n";
    }
    if (data.isMember("mtime_unix_ms")) {
      oss << "mtime_unix_ms: " << (long long)data["mtime_unix_ms"].asInt64() << "\n";
    }
    if (data.isMember("ctime_unix_ms")) {
      oss << "ctime_unix_ms: " << (long long)data["ctime_unix_ms"].asInt64() << "\n";
    }
    if (data.isMember("birthtime_unix_ms")) {
      oss << "birthtime_unix_ms: " << (long long)data["birthtime_unix_ms"].asInt64() << "\n";
    }
    if (data.isMember("is_binary")) {
      oss << "is_binary: " << (data["is_binary"].asBool() ? "true" : "false") << "\n";
    }
    if (data.isMember("count_lines") && data["count_lines"].isBool() && data["count_lines"].asBool()) {
      if (data.isMember("line_count_available") && data["line_count_available"].isBool() && data["line_count_available"].asBool()) {
        if (data.isMember("total_lines")) {
          oss << "total_lines: " << (long long)data["total_lines"].asInt64();
          if (data.isMember("total_lines_truncated") && data["total_lines_truncated"].isBool() && data["total_lines_truncated"].asBool()) {
            oss << " (truncated)";
          }
          oss << "\n";
        }
      } else if (data.isMember("line_count_reason") && data["line_count_reason"].isString()) {
        oss << "total_lines: (unavailable: " << data["line_count_reason"].asString() << ")\n";
      }
    }
    data["output"] = oss.str();
  }
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_list(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_list requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  const std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : ".";
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : false;
  const int max_entries = args.isMember("max_entries") && args["max_entries"].isInt() ? args["max_entries"].asInt() : 200;
  const int max_depth = args.isMember("max_depth") && args["max_depth"].isInt() ? args["max_depth"].asInt() : 4;
  const bool include_hidden = args.isMember("include_hidden") && args["include_hidden"].isBool() ? args["include_hidden"].asBool() : false;
  const bool use_default_excludes =
    !(args.isMember("use_default_excludes") && args["use_default_excludes"].isBool() && args["use_default_excludes"].asBool() == false);
  const bool respect_gitignore =
    args.isMember("respect_gitignore") && args["respect_gitignore"].isBool() ? args["respect_gitignore"].asBool() : false;

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_directory(*resolved, ec)) {
    return write_envelope(false, "not a directory", Json::Value(Json::objectValue));
  }

  std::vector<std::string> exclude_names;
  std::vector<std::string> exclude_globs;
  if (use_default_excludes) {
    // Token-efficiency defaults. These directories commonly contain huge trees and almost never help
    // the model understand project architecture without a targeted question.
    exclude_names.push_back("node_modules");
    exclude_names.push_back(".git");
    exclude_names.push_back("build");
    exclude_names.push_back("dist");
    exclude_names.push_back("out");
    exclude_names.push_back(".cache");
    exclude_names.push_back("__pycache__");
    exclude_names.push_back(".venv");
    exclude_names.push_back("venv");
    exclude_names.push_back(".DS_Store");
  }
  if (args.isMember("exclude_names") && args["exclude_names"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_names"].size(); i++) {
      const auto& v = args["exclude_names"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_names.push_back(s);
      }
    }
  }
  if (args.isMember("exclude_globs") && args["exclude_globs"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_globs"].size(); i++) {
      const auto& v = args["exclude_globs"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_globs.push_back(s);
      }
    }
  }

  Json::Value arr(Json::arrayValue);
  int count = 0;
  int excluded_entries = 0;
  int excluded_dirs = 0;
  int excluded_by_glob = 0;
  int excluded_by_gitignore = 0;
  int excluded_by_symlink = 0;
  if (respect_gitignore) {
    (void)ensure_gitignore_cache(ctx);
  }
  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
    if (!ctx->unrestricted && !ctx->allow_symlinks && is_symlink_path(p)) {
      excluded_by_symlink++;
      return true;
    }
    if (!include_hidden && !name.empty() && name[0] == '.') {
      return true;
    }
    if (!exclude_names.empty() && !name.empty()) {
      for (const auto& ex : exclude_names) {
        if (name == ex) {
          return true;
        }
      }
    }
    if (!exclude_globs.empty()) {
      const std::string entry_path = ctx->unrestricted ? to_generic_string(p.lexically_normal())
                                                       : p.lexically_relative(ctx->root).generic_string();
      if (glob_matches_any(exclude_globs, entry_path)) {
        excluded_by_glob++;
        return true;
      }
    }
    if (respect_gitignore && ctx->gitignore.ready) {
      std::error_code ec2;
      const bool is_dir = std::filesystem::is_directory(p, ec2);
      if (gitignore_is_ignored(ctx, p, is_dir)) {
        excluded_by_gitignore++;
        return true;
      }
    }
    return false;
  };

  if (recursive) {
    for (auto it = std::filesystem::recursive_directory_iterator(*resolved, ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (count >= max_entries) break;
      if (max_depth >= 0 && it.depth() > max_depth) {
        it.disable_recursion_pending();
        continue;
      }
      if (should_skip(it->path())) {
        excluded_entries++;
        if (it->is_directory()) {
          excluded_dirs++;
          it.disable_recursion_pending();
        }
        continue;
      }
      Json::Value e(Json::objectValue);
      if (ctx->unrestricted) {
        e["path"] = to_generic_string(it->path().lexically_normal());
      } else {
        e["path"] = it->path().lexically_relative(ctx->root).generic_string();
      }
      e["type"] = it->is_directory() ? "dir" : (it->is_regular_file() ? "file" : "other");
      if (it->is_regular_file()) {
        std::error_code ec2;
        e["size_bytes"] = (Json::UInt64)it->file_size(ec2);
      }
      arr.append(e);
      count++;
    }
  } else {
    for (auto it = std::filesystem::directory_iterator(*resolved, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
      if (count >= max_entries) break;
      if (should_skip(it->path())) {
        excluded_entries++;
        if (it->is_directory()) excluded_dirs++;
        continue;
      }
      Json::Value e(Json::objectValue);
      if (ctx->unrestricted) {
        e["path"] = to_generic_string(it->path().lexically_normal());
      } else {
        e["path"] = it->path().lexically_relative(ctx->root).generic_string();
      }
      e["type"] = it->is_directory() ? "dir" : (it->is_regular_file() ? "file" : "other");
      if (it->is_regular_file()) {
        std::error_code ec2;
        e["size_bytes"] = (Json::UInt64)it->file_size(ec2);
      }
      arr.append(e);
      count++;
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "fs_list";
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["root_dir"] = to_generic_string(ctx->root);
  data["unrestricted"] = ctx->unrestricted;
  data["recursive"] = recursive;
  data["entries"] = arr;
  data["truncated"] = (count >= max_entries);
  data["max_entries"] = max_entries;
  data["max_depth"] = max_depth;
  data["include_hidden"] = include_hidden;
  data["use_default_excludes"] = use_default_excludes;
  data["respect_gitignore"] = respect_gitignore;
  data["excluded_entries"] = excluded_entries;
  data["excluded_dirs"] = excluded_dirs;
  data["excluded_by_glob"] = excluded_by_glob;
  data["excluded_by_gitignore"] = excluded_by_gitignore;
  data["excluded_by_symlink"] = excluded_by_symlink;
  if (respect_gitignore && ctx->gitignore.ready) {
    data["gitignore_root"] = to_generic_string(ctx->gitignore.root);
    data["gitignore_file"] = to_generic_string(ctx->gitignore.file);
    data["gitignore_rules"] = (Json::UInt64)ctx->gitignore.rules.size();
  }
  {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_names) ex.append(s);
    data["exclude_names"] = ex;
  }
  {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_globs) ex.append(s);
    data["exclude_globs"] = ex;
  }
  {
    std::ostringstream oss;
    oss << "path: " << path << "\n";
    oss << "resolved_path: " << to_generic_string(*resolved) << "\n";
    oss << "recursive: " << (recursive ? "true" : "false") << "\n";
    oss << "use_default_excludes: " << (use_default_excludes ? "true" : "false") << "\n";
    if (!exclude_names.empty()) {
      oss << "exclude_names: ";
      for (size_t i = 0; i < exclude_names.size(); i++) {
        if (i) oss << ", ";
        oss << exclude_names[i];
      }
      oss << "\n";
    }
    if (!exclude_globs.empty()) {
      oss << "exclude_globs: ";
      for (size_t i = 0; i < exclude_globs.size(); i++) {
        if (i) oss << ", ";
        oss << exclude_globs[i];
      }
      oss << "\n";
    }
    if (excluded_entries > 0) {
      oss << "excluded_entries: " << excluded_entries << "\n";
      oss << "excluded_dirs: " << excluded_dirs << "\n";
      if (excluded_by_glob > 0) {
        oss << "excluded_by_glob: " << excluded_by_glob << "\n";
      }
      if (excluded_by_gitignore > 0) {
        oss << "excluded_by_gitignore: " << excluded_by_gitignore << "\n";
      }
    }
    oss << "entries: " << count << (count >= max_entries ? " (truncated)\n" : "\n");
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      const auto& e = arr[i];
      const std::string ep = e.isMember("path") && e["path"].isString() ? e["path"].asString() : "";
      const std::string et = e.isMember("type") && e["type"].isString() ? e["type"].asString() : "";
      oss << "- " << ep;
      if (!et.empty()) oss << " (" << et << ")";
      if (e.isMember("size_bytes") && e["size_bytes"].isUInt64()) {
        oss << " " << (unsigned long long)e["size_bytes"].asUInt64() << " bytes";
      }
      oss << "\n";
    }
    data["output"] = oss.str();
  }
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_find(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_find requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }

  const std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : ".";
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : true;
  const int max_results = args.isMember("max_results") && args["max_results"].isInt() ? args["max_results"].asInt() : 200;
  const int max_depth = args.isMember("max_depth") && args["max_depth"].isInt() ? args["max_depth"].asInt() : 6;
  const bool include_hidden = args.isMember("include_hidden") && args["include_hidden"].isBool() ? args["include_hidden"].asBool() : false;
  const bool use_default_excludes =
    !(args.isMember("use_default_excludes") && args["use_default_excludes"].isBool() && args["use_default_excludes"].asBool() == false);
  const bool respect_gitignore =
    args.isMember("respect_gitignore") && args["respect_gitignore"].isBool() ? args["respect_gitignore"].asBool() : false;
  const std::string type = args.isMember("type") && args["type"].isString() ? args["type"].asString() : "any";
  const std::string name_substring =
    args.isMember("name_substring") && args["name_substring"].isString() ? args["name_substring"].asString() : "";

  if (max_results < 1) return write_envelope(false, "max_results must be >= 1", Json::Value(Json::objectValue));
  if (max_depth < 0) return write_envelope(false, "max_depth must be >= 0", Json::Value(Json::objectValue));

  auto lower_ascii = [](std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };

  std::vector<std::string> exclude_names;
  std::vector<std::string> exclude_globs;
  if (use_default_excludes) {
    exclude_names = {"node_modules", ".git", "build", "dist", "out", ".cache", "__pycache__", ".venv", "venv", ".DS_Store"};
  }
  if (args.isMember("exclude_names") && args["exclude_names"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_names"].size(); i++) {
      const auto& v = args["exclude_names"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_names.push_back(s);
      }
    }
  }
  if (args.isMember("exclude_globs") && args["exclude_globs"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_globs"].size(); i++) {
      const auto& v = args["exclude_globs"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_globs.push_back(s);
      }
    }
  }

  std::vector<std::string> extensions;
  if (args.isMember("extensions") && args["extensions"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["extensions"].size(); i++) {
      const auto& v = args["extensions"][i];
      if (!v.isString()) continue;
      std::string s = v.asString();
      if (s.empty()) continue;
      if (s[0] != '.') s = "." + s;
      extensions.push_back(lower_ascii(s));
    }
  }

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec)) {
    return write_envelope(false, "path does not exist", Json::Value(Json::objectValue));
  }

  int skipped_by_glob = 0;
  int skipped_by_gitignore = 0;
  int skipped_by_symlink = 0;
  if (respect_gitignore) {
    (void)ensure_gitignore_cache(ctx);
  }
  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
    if (!ctx->unrestricted && !ctx->allow_symlinks && is_symlink_path(p)) {
      skipped_by_symlink++;
      return true;
    }
    if (!include_hidden && !name.empty() && name[0] == '.') return true;
    if (!exclude_names.empty() && !name.empty()) {
      for (const auto& ex : exclude_names) {
        if (name == ex) return true;
      }
    }
    if (!exclude_globs.empty()) {
      const std::string entry_path = ctx->unrestricted ? to_generic_string(p.lexically_normal())
                                                       : p.lexically_relative(ctx->root).generic_string();
      if (glob_matches_any(exclude_globs, entry_path)) {
        skipped_by_glob++;
        return true;
      }
    }
    if (respect_gitignore && ctx->gitignore.ready) {
      std::error_code ec2;
      const bool is_dir = std::filesystem::is_directory(p, ec2);
      if (gitignore_is_ignored(ctx, p, is_dir)) {
        skipped_by_gitignore++;
        return true;
      }
    }
    return false;
  };
  const auto ext_matches = [&](const std::filesystem::path& p) -> bool {
    if (extensions.empty()) return true;
    std::string e = p.extension().string();
    if (e.empty()) return false;
    e = lower_ascii(e);
    for (const auto& want : extensions) {
      if (e == want) return true;
    }
    return false;
  };
  const auto type_matches = [&](const std::filesystem::directory_entry& de) -> bool {
    if (type == "any") return true;
    if (type == "file") return de.is_regular_file();
    if (type == "dir") return de.is_directory();
    return true;
  };

  Json::Value entries(Json::arrayValue);
  int added = 0;
  int dirs_skipped = 0;
  int files_seen = 0;
  int files_skipped_by_ext = 0;
  bool truncated = false;

  auto add_entry = [&](const std::filesystem::directory_entry& de) {
    Json::Value e(Json::objectValue);
    if (ctx->unrestricted) {
      e["path"] = to_generic_string(de.path().lexically_normal());
    } else {
      e["path"] = de.path().lexically_relative(ctx->root).generic_string();
    }
    e["type"] = de.is_directory() ? "dir" : (de.is_regular_file() ? "file" : "other");
    if (de.is_regular_file()) {
      std::error_code ec2;
      e["size_bytes"] = (Json::UInt64)de.file_size(ec2);
    }
    entries.append(e);
    added++;
  };

  const auto consider = [&](const std::filesystem::directory_entry& de) {
    if (!type_matches(de)) return;
    if (!name_substring.empty()) {
      const std::string bn = de.path().filename().string();
      if (bn.find(name_substring) == std::string::npos) return;
    }
    if (de.is_regular_file()) {
      files_seen++;
      if (!ext_matches(de.path())) {
        files_skipped_by_ext++;
        return;
      }
    }
    add_entry(de);
  };

  if (std::filesystem::is_regular_file(*resolved, ec)) {
    std::filesystem::directory_entry de(*resolved, ec);
    if (!ec && !should_skip(de.path())) consider(de);
  } else if (std::filesystem::is_directory(*resolved, ec)) {
    if (recursive) {
      for (auto it = std::filesystem::recursive_directory_iterator(*resolved, ec);
           !ec && it != std::filesystem::recursive_directory_iterator();
           ++it) {
        if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
        if (added >= max_results) {
          truncated = true;
          break;
        }
        if (max_depth >= 0 && it.depth() > max_depth) {
          it.disable_recursion_pending();
          continue;
        }
        if (should_skip(it->path())) {
          if (it->is_directory()) {
            dirs_skipped++;
            it.disable_recursion_pending();
          }
          continue;
        }
        consider(*it);
      }
    } else {
      for (auto it = std::filesystem::directory_iterator(*resolved, ec);
           !ec && it != std::filesystem::directory_iterator();
           ++it) {
        if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
        if (added >= max_results) {
          truncated = true;
          break;
        }
        if (should_skip(it->path())) {
          if (it->is_directory()) dirs_skipped++;
          continue;
        }
        consider(*it);
      }
    }
  } else {
    return write_envelope(false, "path is not a file or directory", Json::Value(Json::objectValue));
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "fs_find";
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["root_dir"] = to_generic_string(ctx->root);
  data["unrestricted"] = ctx->unrestricted;
  data["recursive"] = recursive;
  data["max_results"] = max_results;
  data["max_depth"] = max_depth;
  data["include_hidden"] = include_hidden;
  data["use_default_excludes"] = use_default_excludes;
  data["respect_gitignore"] = respect_gitignore;
  data["type"] = type;
  if (!name_substring.empty()) data["name_substring"] = name_substring;
  data["entries"] = entries;
  data["truncated"] = truncated;
  data["dirs_skipped"] = dirs_skipped;
  data["files_seen"] = files_seen;
  data["files_skipped_by_ext"] = files_skipped_by_ext;
  data["skipped_by_glob"] = skipped_by_glob;
  data["skipped_by_gitignore"] = skipped_by_gitignore;
  data["skipped_by_symlink"] = skipped_by_symlink;
  if (respect_gitignore && ctx->gitignore.ready) {
    data["gitignore_root"] = to_generic_string(ctx->gitignore.root);
    data["gitignore_file"] = to_generic_string(ctx->gitignore.file);
    data["gitignore_rules"] = (Json::UInt64)ctx->gitignore.rules.size();
  }
  if (!extensions.empty()) {
    Json::Value exts(Json::arrayValue);
    for (const auto& s : extensions) exts.append(s);
    data["extensions"] = exts;
  }
  if (!exclude_globs.empty()) {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_globs) ex.append(s);
    data["exclude_globs"] = ex;
  }

  {
    std::ostringstream oss;
    oss << "path: " << path << "\n";
    oss << "resolved_path: " << to_generic_string(*resolved) << "\n";
    oss << "recursive: " << (recursive ? "true" : "false") << "\n";
    oss << "type: " << type << "\n";
    if (!name_substring.empty()) oss << "name_substring: " << name_substring << "\n";
    if (!extensions.empty()) {
      oss << "extensions: ";
      for (size_t i = 0; i < extensions.size(); i++) {
        if (i) oss << ", ";
        oss << extensions[i];
      }
      oss << "\n";
    }
    if (!exclude_globs.empty()) {
      oss << "exclude_globs: ";
      for (size_t i = 0; i < exclude_globs.size(); i++) {
        if (i) oss << ", ";
        oss << exclude_globs[i];
      }
      oss << "\n";
    }
    if (respect_gitignore && ctx->gitignore.ready) {
      oss << "respect_gitignore: true\n";
      oss << "gitignore_file: " << to_generic_string(ctx->gitignore.file) << "\n";
      oss << "gitignore_rules: " << ctx->gitignore.rules.size() << "\n";
    }
    oss << "use_default_excludes: " << (use_default_excludes ? "true" : "false") << "\n";
    oss << "entries: " << added << (truncated ? " (truncated)\n" : "\n");
    for (Json::ArrayIndex i = 0; i < entries.size() && i < 100; i++) {
      const auto& e = entries[i];
      const std::string ep = e.isMember("path") && e["path"].isString() ? e["path"].asString() : "";
      const std::string et = e.isMember("type") && e["type"].isString() ? e["type"].asString() : "";
      oss << "- " << ep;
      if (!et.empty()) oss << " (" << et << ")";
      if (e.isMember("size_bytes") && e["size_bytes"].isUInt64()) {
        oss << " " << (unsigned long long)e["size_bytes"].asUInt64() << " bytes";
      }
      oss << "\n";
    }
    data["output"] = oss.str();
  }

  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_read(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_read requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }

  const std::string path = args["path"].asString();
  const int start_line = args.isMember("start_line") && args["start_line"].isInt() ? args["start_line"].asInt() : 1;
  const int max_lines = args.isMember("max_lines") && args["max_lines"].isInt() ? args["max_lines"].asInt() : 200;
  const int end_line = args.isMember("end_line") && args["end_line"].isInt() ? args["end_line"].asInt() : 0;
  const int max_chars = args.isMember("max_chars") && args["max_chars"].isInt() ? args["max_chars"].asInt() : 20000;
  const bool with_line_numbers = args.isMember("with_line_numbers") && args["with_line_numbers"].isBool() ? args["with_line_numbers"].asBool() : false;

  if (start_line < 1) {
    return write_envelope(false, "start_line must be >= 1", Json::Value(Json::objectValue));
  }
  if (max_lines < 1) {
    return write_envelope(false, "max_lines must be >= 1", Json::Value(Json::objectValue));
  }

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_regular_file(*resolved, ec)) {
    return write_envelope(false, "not a regular file", Json::Value(Json::objectValue));
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "fs_read";
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["root_dir"] = to_generic_string(ctx->root);
  data["unrestricted"] = ctx->unrestricted;
  data["start_line"] = start_line;
  data["max_lines"] = max_lines;
  if (end_line > 0) data["end_line"] = end_line;
  data["max_chars"] = max_chars;
  data["with_line_numbers"] = with_line_numbers;

  const uintmax_t sz = std::filesystem::file_size(*resolved, ec);
  if (!ec) data["size_bytes"] = (Json::UInt64)sz;
  const auto mtime = std::filesystem::last_write_time(*resolved, ec);
  if (!ec) data["mtime_unix_ms"] = (Json::Int64)file_time_to_unix_ms(mtime);
  {
    int64_t ctime_ms = 0;
    int64_t birth_ms = 0;
    if (posix_stat_times_best_effort(*resolved, &ctime_ms, &birth_ms)) {
      if (ctime_ms > 0) data["ctime_unix_ms"] = (Json::Int64)ctime_ms;
      if (birth_ms > 0) data["birthtime_unix_ms"] = (Json::Int64)birth_ms;
    }
  }

  const bool is_bin = is_probably_binary(*resolved);
  data["is_binary"] = is_bin;
  if (is_bin) {
    data["content"] = "";
    data["output"] = "";
    data["truncated"] = true;
    data["truncated_reason"] = "binary_file";
    return write_envelope(true, "", data);
  }

  std::ifstream in(*resolved);
  if (!in.is_open()) {
    return write_envelope(false, "failed to open", Json::Value(Json::objectValue));
  }

  std::ostringstream out;
  int line_no = 0;
  int returned = 0;
  bool truncated = false;
  bool has_more = false;
  std::string truncated_reason;
  bool stopped_due_to_end_line = false;
  std::string line;
  while (std::getline(in, line)) {
    line_no++;
    if (line_no < start_line) {
      continue;
    }
    if (end_line > 0 && line_no > end_line) {
      has_more = true;
      stopped_due_to_end_line = true;
      break;
    }
    if (returned >= max_lines) {
      truncated = true;
      has_more = true;
      truncated_reason = "max_lines";
      break;
    }
    std::string chunk = line;
    if (with_line_numbers) {
      chunk = std::to_string(line_no) + ": " + chunk;
    }
    // Add newline back (getline strips it).
    chunk.push_back('\n');
    if ((int)out.tellp() + (int)chunk.size() > max_chars) {
      truncated = true;
      has_more = true;
      truncated_reason = "max_chars";
      break;
    }
    out << chunk;
    returned++;
  }

  data["lines_returned"] = returned;
  data["last_line"] = returned > 0 ? (start_line + returned - 1) : (start_line - 1);
  data["has_more"] = has_more;
  data["next_start_line"] = (Json::Int64)(start_line + returned);
  data["truncated"] = truncated;
  if (truncated && !truncated_reason.empty()) data["truncated_reason"] = truncated_reason;
  if (stopped_due_to_end_line) data["stopped_due_to"] = "end_line";

  // Optional metadata: if we didn't reach EOF due to truncation or end_line, estimate total lines.
  // This is bounded by file size so it stays fast and predictable.
  if (!has_more) {
    data["total_lines"] = line_no;
    data["lines_remaining"] = 0;
  } else if (!ec && sz > 0 && sz <= (uintmax_t)(1024 * 1024)) {
    // Continue scanning to EOF without buffering content, just count remaining lines.
    while (std::getline(in, line)) {
      line_no++;
    }
    data["total_lines"] = line_no;
    const int last_line = returned > 0 ? (start_line + returned - 1) : (start_line - 1);
    data["lines_remaining"] = std::max(0, line_no - last_line);
    data["total_lines_estimated"] = true;
  }
  data["content"] = out.str();
  data["output"] = data["content"];
  data["content_bytes"] = (Json::UInt64)data["content"].asString().size();

  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_text_search(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"text_search requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args.isMember("query") || !args["query"].isString() || args["query"].asString().empty()) {
    return write_envelope(false, "missing string field 'query'", Json::Value(Json::objectValue));
  }

  const std::string query = args["query"].asString();
  const std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : ".";
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : true;
  const bool case_sensitive =
    args.isMember("case_sensitive") && args["case_sensitive"].isBool() ? args["case_sensitive"].asBool() : false;
  const int max_results = args.isMember("max_results") && args["max_results"].isInt() ? args["max_results"].asInt() : 200;
  const int max_file_bytes =
    args.isMember("max_file_bytes") && args["max_file_bytes"].isInt() ? args["max_file_bytes"].asInt() : 512 * 1024;
  const int max_line_chars =
    args.isMember("max_line_chars") && args["max_line_chars"].isInt() ? args["max_line_chars"].asInt() : 400;
  const bool include_hidden =
    args.isMember("include_hidden") && args["include_hidden"].isBool() ? args["include_hidden"].asBool() : false;
  const bool use_default_excludes =
    !(args.isMember("use_default_excludes") && args["use_default_excludes"].isBool() && args["use_default_excludes"].asBool() == false);
  const bool respect_gitignore =
    args.isMember("respect_gitignore") && args["respect_gitignore"].isBool() ? args["respect_gitignore"].asBool() : false;

  auto lower_ascii = [](std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  const std::string q = case_sensitive ? query : lower_ascii(query);

  std::vector<std::string> extensions;
  if (args.isMember("extensions") && args["extensions"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["extensions"].size(); i++) {
      const auto& v = args["extensions"][i];
      if (!v.isString()) continue;
      std::string s = v.asString();
      if (s.empty()) continue;
      if (s[0] != '.') s = "." + s;
      extensions.push_back(lower_ascii(s));
    }
  }

  if (max_results < 1) {
    return write_envelope(false, "max_results must be >= 1", Json::Value(Json::objectValue));
  }
  if (max_file_bytes < 0) {
    return write_envelope(false, "max_file_bytes must be >= 0", Json::Value(Json::objectValue));
  }
  if (max_line_chars < 1) {
    return write_envelope(false, "max_line_chars must be >= 1", Json::Value(Json::objectValue));
  }

  std::vector<std::string> exclude_names;
  std::vector<std::string> exclude_globs;
  if (use_default_excludes) {
    exclude_names.push_back("node_modules");
    exclude_names.push_back(".git");
    exclude_names.push_back("build");
    exclude_names.push_back("dist");
    exclude_names.push_back("out");
    exclude_names.push_back(".cache");
    exclude_names.push_back("__pycache__");
    exclude_names.push_back(".venv");
    exclude_names.push_back("venv");
    exclude_names.push_back(".DS_Store");
  }
  if (args.isMember("exclude_names") && args["exclude_names"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_names"].size(); i++) {
      const auto& v = args["exclude_names"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_names.push_back(s);
      }
    }
  }
  if (args.isMember("exclude_globs") && args["exclude_globs"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["exclude_globs"].size(); i++) {
      const auto& v = args["exclude_globs"][i];
      if (v.isString()) {
        const std::string s = v.asString();
        if (!s.empty()) exclude_globs.push_back(s);
      }
    }
  }

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec)) {
    return write_envelope(false, "path does not exist", Json::Value(Json::objectValue));
  }

  const auto ext_matches = [&](const std::filesystem::path& p) -> bool {
    if (extensions.empty()) return true;
    std::string e = p.extension().string();
    if (e.empty()) return false;
    e = lower_ascii(e);
    for (const auto& want : extensions) {
      if (e == want) return true;
    }
    return false;
  };

  int skipped_by_glob = 0;
  int skipped_by_gitignore = 0;
  int skipped_by_symlink = 0;
  if (respect_gitignore) {
    (void)ensure_gitignore_cache(ctx);
  }
  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
    if (!ctx->unrestricted && !ctx->allow_symlinks && is_symlink_path(p)) {
      skipped_by_symlink++;
      return true;
    }
    if (!include_hidden && !name.empty() && name[0] == '.') return true;
    if (!exclude_names.empty() && !name.empty()) {
      for (const auto& ex : exclude_names) {
        if (name == ex) return true;
      }
    }
    if (!exclude_globs.empty()) {
      const std::string entry_path = ctx->unrestricted ? to_generic_string(p.lexically_normal())
                                                       : p.lexically_relative(ctx->root).generic_string();
      if (glob_matches_any(exclude_globs, entry_path)) {
        skipped_by_glob++;
        return true;
      }
    }
    if (respect_gitignore && ctx->gitignore.ready) {
      std::error_code ec2;
      const bool is_dir = std::filesystem::is_directory(p, ec2);
      if (gitignore_is_ignored(ctx, p, is_dir)) {
        skipped_by_gitignore++;
        return true;
      }
    }
    return false;
  };

  Json::Value matches(Json::arrayValue);
  int files_scanned = 0;
  int files_skipped_binary = 0;
  int files_skipped_too_large = 0;
  int files_skipped_by_ext = 0;
  int dirs_skipped = 0;
  bool truncated = false;

  auto add_match = [&](const std::filesystem::path& file_path, int line_no, int col_1based, const std::string& snippet) {
    Json::Value m(Json::objectValue);
    if (ctx->unrestricted) {
      m["path"] = to_generic_string(file_path.lexically_normal());
    } else {
      m["path"] = file_path.lexically_relative(ctx->root).generic_string();
    }
    m["line"] = line_no;
    m["column"] = col_1based;
    m["snippet"] = snippet;
    matches.append(m);
  };

  auto scan_file = [&](const std::filesystem::path& file_path) {
    if (is_cancelled(ctx)) return;
    if (!ext_matches(file_path)) {
      files_skipped_by_ext++;
      return;
    }
    std::error_code ec2;
    const uintmax_t sz = std::filesystem::file_size(file_path, ec2);
    if (!ec2 && max_file_bytes >= 0 && sz > (uintmax_t)max_file_bytes) {
      files_skipped_too_large++;
      return;
    }
    if (is_probably_binary(file_path)) {
      files_skipped_binary++;
      return;
    }

    std::ifstream in(file_path);
    if (!in.is_open()) return;
    files_scanned++;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
      if (is_cancelled(ctx)) return;
      line_no++;
      std::string hay = case_sensitive ? line : lower_ascii(line);
      const size_t pos = hay.find(q);
      if (pos == std::string::npos) continue;
      std::string snippet = line;
      if ((int)snippet.size() > max_line_chars) {
        snippet.resize((size_t)max_line_chars);
        snippet += "...(truncated)";
      }
      add_match(file_path, line_no, (int)pos + 1, snippet);
      if ((int)matches.size() >= max_results) {
        truncated = true;
        return;
      }
    }
  };

  auto scan_path = [&](const std::filesystem::path& p) {
    std::error_code ec3;
    const auto st = std::filesystem::status(p, ec3);
    if (ec3) return;
    if (std::filesystem::is_regular_file(st)) {
      if (!should_skip(p)) {
        scan_file(p);
      }
      return;
    }
    if (!std::filesystem::is_directory(st)) return;

    if (!recursive) {
      for (auto it = std::filesystem::directory_iterator(p, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (is_cancelled(ctx)) return;
        if (truncated) return;
        if (should_skip(it->path())) {
          if (it->is_directory()) dirs_skipped++;
          continue;
        }
        const auto st2 = it->status(ec3);
        if (ec3) continue;
        if (std::filesystem::is_regular_file(st2)) scan_file(it->path());
      }
      return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(p, ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (is_cancelled(ctx)) return;
      if (truncated) return;
      if (should_skip(it->path())) {
        if (it->is_directory()) {
          dirs_skipped++;
          it.disable_recursion_pending();
        }
        continue;
      }
      if (it->is_regular_file()) {
        scan_file(it->path());
      }
    }
  };

  scan_path(*resolved);

  Json::Value data(Json::objectValue);
  data["tool"] = "text_search";
  data["query"] = query;
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["root_dir"] = to_generic_string(ctx->root);
  data["unrestricted"] = ctx->unrestricted;
  data["recursive"] = recursive;
  data["case_sensitive"] = case_sensitive;
  data["include_hidden"] = include_hidden;
  data["use_default_excludes"] = use_default_excludes;
  {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_names) ex.append(s);
    data["exclude_names"] = ex;
  }
  if (!exclude_globs.empty()) {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_globs) ex.append(s);
    data["exclude_globs"] = ex;
  }
  data["respect_gitignore"] = respect_gitignore;
  data["skipped_by_gitignore"] = skipped_by_gitignore;
  if (respect_gitignore && ctx->gitignore.ready) {
    data["gitignore_root"] = to_generic_string(ctx->gitignore.root);
    data["gitignore_file"] = to_generic_string(ctx->gitignore.file);
    data["gitignore_rules"] = (Json::UInt64)ctx->gitignore.rules.size();
  }
  data["max_results"] = max_results;
  data["max_file_bytes"] = max_file_bytes;
  data["max_line_chars"] = max_line_chars;
  data["files_scanned"] = files_scanned;
  data["files_skipped_binary"] = files_skipped_binary;
  data["files_skipped_too_large"] = files_skipped_too_large;
  data["files_skipped_by_ext"] = files_skipped_by_ext;
  data["dirs_skipped"] = dirs_skipped;
  data["skipped_by_glob"] = skipped_by_glob;
  data["skipped_by_symlink"] = skipped_by_symlink;
  data["matches"] = matches;
  data["matches_count"] = (Json::UInt64)matches.size();
  data["truncated"] = truncated;
  if (!extensions.empty()) {
    Json::Value exts(Json::arrayValue);
    for (const auto& s : extensions) exts.append(s);
    data["extensions"] = exts;
  }

  {
    std::ostringstream oss;
    oss << "query: " << query << "\n";
    oss << "path: " << path << "\n";
    oss << "resolved_path: " << to_generic_string(*resolved) << "\n";
    oss << "recursive: " << (recursive ? "true" : "false") << "\n";
    oss << "case_sensitive: " << (case_sensitive ? "true" : "false") << "\n";
    oss << "include_hidden: " << (include_hidden ? "true" : "false") << "\n";
    if (!extensions.empty()) {
      oss << "extensions: ";
      for (size_t i = 0; i < extensions.size(); i++) {
        if (i) oss << ", ";
        oss << extensions[i];
      }
      oss << "\n";
    }
    if (!exclude_globs.empty()) {
      oss << "exclude_globs: ";
      for (size_t i = 0; i < exclude_globs.size(); i++) {
        if (i) oss << ", ";
        oss << exclude_globs[i];
      }
      oss << "\n";
    }
    if (respect_gitignore && ctx->gitignore.ready) {
      oss << "respect_gitignore: true\n";
      oss << "gitignore_file: " << to_generic_string(ctx->gitignore.file) << "\n";
      oss << "gitignore_rules: " << ctx->gitignore.rules.size() << "\n";
    }
    oss << "use_default_excludes: " << (use_default_excludes ? "true" : "false") << "\n";
    oss << "matches: " << (unsigned long long)matches.size() << (truncated ? " (truncated)\n" : "\n");
    oss << "files_scanned: " << files_scanned << "\n";
    if (files_skipped_binary) oss << "files_skipped_binary: " << files_skipped_binary << "\n";
    if (files_skipped_too_large) oss << "files_skipped_too_large: " << files_skipped_too_large << "\n";
    if (files_skipped_by_ext) oss << "files_skipped_by_ext: " << files_skipped_by_ext << "\n";
    if (dirs_skipped) oss << "dirs_skipped: " << dirs_skipped << "\n";
    if (skipped_by_glob) oss << "skipped_by_glob: " << skipped_by_glob << "\n";
    if (skipped_by_gitignore) oss << "skipped_by_gitignore: " << skipped_by_gitignore << "\n";
    for (Json::ArrayIndex i = 0; i < matches.size() && i < 50; i++) {
      const auto& m = matches[i];
      const std::string mp = m.isMember("path") && m["path"].isString() ? m["path"].asString() : "";
      const int ln = m.isMember("line") && m["line"].isInt() ? m["line"].asInt() : 0;
      const int col = m.isMember("column") && m["column"].isInt() ? m["column"].asInt() : 0;
      const std::string sn = m.isMember("snippet") && m["snippet"].isString() ? m["snippet"].asString() : "";
      oss << "- " << mp << ":" << ln << ":" << col << " " << sn << "\n";
    }
    data["output"] = oss.str();
  }

  return write_envelope(true, is_cancelled(ctx) ? "cancelled" : "", data);
#endif
}

static agent_status_t host_tools_execute(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!vctx || !tool_name || !arguments_json || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  HostToolCtx* ctx = (HostToolCtx*)vctx;
  const std::string name(tool_name);
  if (ctx->policy == HostToolsetPolicyMode::ReadOnly) {
    if (name == "shell_exec" || name == "proc_exec" || name == "file_apply_patch" || name == "camera_capture") {
#if !defined(AGENT_HAVE_JSONCPP)
      return set_result(out_result, "{\"ok\":false,\"error\":\"tool disabled by policy\",\"data\":{}}");
#else
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool disabled by policy";
      Json::Value d(Json::objectValue);
      d["tool_name"] = name;
      d["policy"] = "readonly";
      o["data"] = d;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      return set_result(out_result, Json::writeString(wb, o));
#endif
    }
  }
  if (!ctx->exec_enabled) {
    if (name == "shell_exec" || name == "proc_exec" || name == "camera_capture") {
#if !defined(AGENT_HAVE_JSONCPP)
      return set_result(out_result, "{\"ok\":false,\"error\":\"tool disabled by sandbox\",\"data\":{}}");
#else
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool disabled by sandbox";
      Json::Value d(Json::objectValue);
      d["tool_name"] = name;
      d["sandbox"] = "scoped";
      o["data"] = d;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      return set_result(out_result, Json::writeString(wb, o));
#endif
    }
  }
  if (name == "shell_exec") {
    return tool_shell_exec(ctx, arguments_json, out_result);
  }
  if (name == "proc_exec") {
    return tool_proc_exec(ctx, arguments_json, out_result);
  }
  if (name == "file_apply_patch") {
    return tool_file_apply_patch(ctx, arguments_json, out_result);
  }
  if (name == "fs_stat") {
    return tool_fs_stat(ctx, arguments_json, out_result);
  }
  if (name == "fs_list") {
    return tool_fs_list(ctx, arguments_json, out_result);
  }
  if (name == "fs_find") {
    return tool_fs_find(ctx, arguments_json, out_result);
  }
  if (name == "fs_read") {
    return tool_fs_read(ctx, arguments_json, out_result);
  }
  if (name == "text_search") {
    return tool_text_search(ctx, arguments_json, out_result);
  }
  if (name == "artifact_register") {
    return tool_artifact_register(ctx, arguments_json, out_result);
  }
  if (name == "ui_action") {
    return tool_ui_action(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_event") {
    return tool_ui_wait_event(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_any") {
    return tool_ui_wait_any(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_all") {
    return tool_ui_wait_all(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_event") {
    return tool_client_wait_event(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_any") {
    return tool_client_wait_any(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_all") {
    return tool_client_wait_all(ctx, arguments_json, out_result);
  }
  if (name == "client_peek") {
    return tool_client_peek(ctx, arguments_json, out_result);
  }
  if (name == "camera_capture") {
    return tool_camera_capture(ctx, arguments_json, out_result);
  }
  // Keep the response machine-readable so the LLM can reason about failures.
  return set_result(out_result, "{\"ok\":false,\"error\":\"unknown tool\",\"data\":{}}");
}

static agent_status_t add_tool(agent_tool_registry_t* r, const char* name, const char* desc, const char* params_json) {
  return agent_tool_registry_add(r, name, desc, params_json);
}

} // namespace

agent_status_t toolset_host_create(const HostToolsetConfig& cfg, agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor) {
  if (!out_registry || !out_executor) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_registry = nullptr;
  out_executor->ctx = nullptr;
  out_executor->execute = nullptr;

  agent_tool_registry_t* r = nullptr;
  HostToolCtx* ctx = nullptr;
  agent_status_t st = agent_tool_registry_create(&r);
  if (st != AGENT_OK) {
    return st;
  }

  // Process exec and patch (host-only tooling).
  if (cfg.policy == HostToolsetPolicyMode::Full) {
    if (cfg.enable_process_exec) {
      st = add_tool(
        r,
        "shell_exec",
        "Execute /bin/sh -lc <cmd>. Returns JSON envelope: {ok, error?, data:{exit_code, timed_out, output}}. ok is a hint; judge success from output.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"cmd\":{\"type\":\"string\"},"
        "  \"timeout_ms\":{\"type\":\"integer\"},"
        "  \"max_output_bytes\":{\"type\":\"integer\"}"
        "},"
        "\"required\":[\"cmd\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;

      st = add_tool(
        r,
        "proc_exec",
        "Execute a process without a shell (posix_spawnp). Returns JSON envelope: {ok, error?, data:{argv, exit_code, timed_out, truncated, output}}.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "  \"timeout_ms\":{\"type\":\"integer\"},"
        "  \"max_output_bytes\":{\"type\":\"integer\"}"
        "},"
        "\"required\":[\"argv\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;
    }

    st = add_tool(
      r,
      "file_apply_patch",
      "Apply a unified-diff patch (uses `git apply`). Returns JSON envelope: {ok, error?, data:{patch, check, apply?}}.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"patch\":{\"type\":\"string\"},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_output_bytes\":{\"type\":\"integer\"},"
      "  \"unsafe_paths\":{\"type\":\"boolean\",\"description\":\"Only honored in unrestricted mode (root_dir empty).\"}"
      "},"
      "\"required\":[\"patch\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;
  }

  // Read-only filesystem tools (host-only):
  // These exist to reduce token waste by bounding outputs and supporting pagination.
  st = add_tool(
    r,
    "fs_stat",
    "Stat a file or directory (host-side). Returns JSON envelope with metadata and a human-readable `data.output`.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"File/directory path (relative to tools root unless yolo/unrestricted).\"},"
    "  \"count_lines\":{\"type\":\"boolean\",\"description\":\"When true, count lines for small text files (bounded).\"},"
    "  \"max_count_bytes\":{\"type\":\"integer\",\"description\":\"Only count lines when file size <= this many bytes (default: 2097152).\"},"
    "  \"max_count_lines\":{\"type\":\"integer\",\"description\":\"Stop counting after this many lines (default: 200000).\"}"
    "},"
    "\"required\":[\"path\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_list",
    "List a directory (host-side). Prefer this over `ls`/`find` when you need bounded output for token efficiency.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"Directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\"},"
    "  \"max_entries\":{\"type\":\"integer\",\"description\":\"Max entries to return (default: 200)\"},"
    "  \"max_depth\":{\"type\":\"integer\",\"description\":\"Max recursion depth when recursive=true (default: 4)\"},"
    "  \"include_hidden\":{\"type\":\"boolean\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned entry paths.\"}"
    "}"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_find",
    "Find files/dirs under a path with bounded output (host-side). Prefer this over `find`/`tree` for predictable token usage.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"File or directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\",\"description\":\"When path is a directory, recurse (default: true).\"},"
    "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max entries to return (default: 200)\"},"
    "  \"max_depth\":{\"type\":\"integer\",\"description\":\"Max recursion depth when recursive=true (default: 6)\"},"
    "  \"include_hidden\":{\"type\":\"boolean\",\"description\":\"Include dotfiles (default: false).\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"type\":{\"type\":\"string\",\"description\":\"Entry type filter: any|file|dir (default: any).\"},"
    "  \"name_substring\":{\"type\":\"string\",\"description\":\"Optional substring filter on basename.\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned entry paths.\"}"
    "}"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_read",
    "Read a text file with bounded output + pagination. Prefer this over `cat`/`sed` when you need predictable token usage.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\"},"
    "  \"start_line\":{\"type\":\"integer\",\"description\":\"1-based starting line (default: 1)\"},"
    "  \"end_line\":{\"type\":\"integer\",\"description\":\"Optional 1-based end line (inclusive). 0 means unset.\"},"
    "  \"max_lines\":{\"type\":\"integer\",\"description\":\"Max lines to return (default: 200)\"},"
    "  \"max_chars\":{\"type\":\"integer\",\"description\":\"Max characters to return (default: 20000)\"},"
    "  \"with_line_numbers\":{\"type\":\"boolean\"}"
    "},"
    "\"required\":[\"path\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "text_search",
    "Search for a substring in files under a path (token-safe, bounded output). Prefer this over `grep -R` for predictable output size.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"query\":{\"type\":\"string\",\"description\":\"Substring to search for.\"},"
    "  \"path\":{\"type\":\"string\",\"description\":\"File or directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\",\"description\":\"When path is a directory, recurse (default: true).\"},"
    "  \"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Case-sensitive search (default: false).\"},"
    "  \"include_hidden\":{\"type\":\"boolean\",\"description\":\"Include dotfiles (default: false).\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max matches to return (default: 200).\"},"
    "  \"max_file_bytes\":{\"type\":\"integer\",\"description\":\"Skip files larger than this many bytes (default: 524288). 0 disables size limit.\"},"
    "  \"max_line_chars\":{\"type\":\"integer\",\"description\":\"Max chars per snippet line (default: 400).\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned match paths.\"}"
    "},"
    "\"required\":[\"query\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "artifact_register",
    "Register a host file (image/audio/video/etc) as an artifact for the UI to render. Returns JSON envelope with data.artifact metadata and playback hints.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"description\":\"image|audio|video|text|file\"},\"mime\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"autoplay\":{\"type\":\"boolean\"},\"repeat\":{\"type\":\"integer\"}},\"required\":[\"path\"]}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "ui_action",
    "Request a UI action (no host side effects). Returns JSON envelope with data.action; the tool loop emits a derived ui_action event to the UI.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"type\":{\"type\":\"string\",\"description\":\"Action type (e.g. notify, play_audio).\"},"
    "  \"title\":{\"type\":\"string\"},"
    "  \"message\":{\"type\":\"string\"},"
    "  \"path\":{\"type\":\"string\",\"description\":\"For media actions like play_audio.\"},"
    "  \"mime\":{\"type\":\"string\"},"
    "  \"repeat\":{\"type\":\"integer\"},"
    "  \"autoplay\":{\"type\":\"boolean\"},"
    "  \"rpc_id\":{\"type\":\"string\",\"description\":\"Correlation id for client_rpc/collab_rpc.\"},"
    "  \"rpc\":{\"type\":\"object\",\"description\":\"Client RPC request payload (kind/args/etc).\"},"
    "  \"side_effects\":{\"type\":\"boolean\",\"description\":\"Advisory: indicates the action expects client-side side effects.\"},"
    "  \"auto_run\":{\"type\":\"boolean\",\"description\":\"Request client auto-run when permitted.\"},"
    "  \"auto\":{\"type\":\"boolean\",\"description\":\"Alias for auto_run.\"},"
    "  \"probe_id\":{\"type\":\"string\",\"description\":\"Legacy correlation id for client_probe.\"},"
    "  \"probe\":{\"type\":\"object\",\"description\":\"Legacy probe request payload.\"},"
    "  \"query_id\":{\"type\":\"string\",\"description\":\"Correlation id for request_client_state.\"}"
    "},"
    "\"required\":[\"type\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  if (!cfg.sessions_root_dir.empty() && !cfg.session_id.empty()) {
    st = add_tool(
      r,
      "ui_wait_event",
      "Wait for a client event (posted via agentd /api/v1/session/client_event) for this session. Useful for acknowledgements like audio_play_finished. (Deprecated name; prefer client_wait_event.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"type\":{\"type\":\"string\",\"description\":\"Client event type to wait for (e.g. audio_play_finished).\"},"
      "  \"client_id\":{\"type\":\"string\",\"description\":\"Optional filter for payload.client.id (e.g. webui, slack).\"},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"after_unix_ms\":{\"type\":\"integer\",\"description\":\"Ignore events older than this timestamp (optional).\"},"
      "  \"path\":{\"type\":\"string\",\"description\":\"Optional filter for payload.data.path.\"},"
      "  \"data_match\":{\"type\":\"object\",\"description\":\"Optional partial-match filter applied to payload.data (nested objects supported; arrays match exactly).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"}"
      "},"
      "\"required\":[\"type\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "ui_wait_any",
      "Wait until any of multiple client event predicates matches (OR join). (Deprecated name; prefer client_wait_any.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"description\":\"List of event predicates to match.\",\"items\":{"
        "    \"type\":\"object\","
        "    \"properties\":{"
          "      \"type\":{\"type\":\"string\"},"
          "      \"client_id\":{\"type\":\"string\"},"
          "      \"after_unix_ms\":{\"type\":\"integer\"},"
          "      \"path\":{\"type\":\"string\"},"
          "      \"data_match\":{\"type\":\"object\"}"
        "    },"
        "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider (default: daemon/session store default).\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "ui_wait_all",
      "Wait until all of multiple client event predicates match (AND join). (Deprecated name; prefer client_wait_all.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"description\":\"List of event predicates to match.\",\"items\":{"
        "    \"type\":\"object\","
        "    \"properties\":{"
          "      \"type\":{\"type\":\"string\"},"
          "      \"client_id\":{\"type\":\"string\"},"
          "      \"after_unix_ms\":{\"type\":\"integer\"},"
          "      \"path\":{\"type\":\"string\"},"
          "      \"data_match\":{\"type\":\"object\"}"
        "    },"
        "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider (default: daemon/session store default).\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_event",
      "Wait for a client event (posted via agentd /api/v1/session/client_event) for this session. This is the preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"type\":{\"type\":\"string\"},"
      "  \"client_id\":{\"type\":\"string\"},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"after_unix_ms\":{\"type\":\"integer\"},"
      "  \"path\":{\"type\":\"string\"},"
      "  \"data_match\":{\"type\":\"object\"},"
      "  \"max_bytes\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"type\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_any",
      "Wait until any predicate matches (OR join). Preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"items\":{"
      "    \"type\":\"object\","
      "    \"properties\":{"
      "      \"type\":{\"type\":\"string\"},"
      "      \"client_id\":{\"type\":\"string\"},"
      "      \"after_unix_ms\":{\"type\":\"integer\"},"
      "      \"path\":{\"type\":\"string\"},"
      "      \"data_match\":{\"type\":\"object\"}"
      "    },"
      "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_bytes\":{\"type\":\"integer\"},"
      "  \"max_files\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_all",
      "Wait until all predicates match (AND join). Preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"items\":{"
      "    \"type\":\"object\","
      "    \"properties\":{"
      "      \"type\":{\"type\":\"string\"},"
      "      \"client_id\":{\"type\":\"string\"},"
      "      \"after_unix_ms\":{\"type\":\"integer\"},"
      "      \"path\":{\"type\":\"string\"},"
      "      \"data_match\":{\"type\":\"object\"}"
      "    },"
      "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_bytes\":{\"type\":\"integer\"},"
      "  \"max_files\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_peek",
      "Probe recent client event state for this session (non-blocking). Useful for reasoning about client environment/state without waiting.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"client_id\":{\"type\":\"string\",\"description\":\"Optional filter for client.id.\"},"
      "  \"event_type\":{\"type\":\"string\",\"description\":\"Optional filter for payload.type (e.g. client_state).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Bytes to scan from the tail (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider.\"},"
      "  \"include_data\":{\"type\":\"boolean\",\"description\":\"When true, include a bounded view of the last event payload.\"},"
      "  \"max_data_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes of last_event.data before it is summarized/truncated.\"}"
      "}"
      "}"
    );
    if (st != AGENT_OK) goto fail;
  }

  // Camera capture:
  // - If exec is disabled (scoped/safe runs), the tool still exists but will default to backend=mock.
  // - If exec is enabled, backend=ffmpeg is available (subject to local ffmpeg/device permissions).
  if (cfg.policy == HostToolsetPolicyMode::Full) {
    st = add_tool(
      r,
      "camera_capture",
      "Capture a single image from the host camera (or mock backend). In scoped/no-exec runs, prefer backend=mock. Returns JSON envelope with data.artifact for UI rendering; the tool loop emits a derived artifact event.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"path\":{\"type\":\"string\",\"description\":\"Output file path (relative to tools root in scoped mode).\"},"
      "  \"backend\":{\"type\":\"string\",\"description\":\"auto|ffmpeg|mock\"},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"title\":{\"type\":\"string\"},"
      "  \"register_artifact\":{\"type\":\"boolean\"},"
      "  \"notify\":{\"type\":\"boolean\"}"
      "}"
      "}"
    );
    if (st != AGENT_OK) goto fail;
  }

  // Executor context (owned by host; for CLI we just heap-allocate).
  ctx = new (std::nothrow) HostToolCtx();
  if (!ctx) {
    st = AGENT_ERR_OOM;
    goto fail;
  }
  ctx->unrestricted = cfg.root_dir.empty();
  ctx->root = cfg.root_dir.empty() ? std::filesystem::current_path()
                                   : std::filesystem::path(cfg.root_dir);
  ctx->policy = cfg.policy;
  ctx->exec_enabled = (cfg.policy == HostToolsetPolicyMode::Full) && cfg.enable_process_exec;
  ctx->allow_symlinks = cfg.allow_symlinks;
  ctx->should_cancel = cfg.should_cancel;
  ctx->should_cancel_ctx = cfg.should_cancel_ctx;
  if (!cfg.sessions_root_dir.empty()) {
    ctx->sessions_root_dir = std::filesystem::path(cfg.sessions_root_dir);
  }
  ctx->session_id = cfg.session_id;
  {
    // Normalize to reduce symlink/canonical mismatch (e.g. /var vs /private/var on macOS).
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(ctx->root, ec);
    if (!ec) {
      ctx->root = canon;
    } else {
      ctx->root = ctx->root.lexically_normal();
    }
  }

  out_executor->ctx = ctx;
  out_executor->execute = host_tools_execute;
  *out_registry = r;
  return AGENT_OK;

fail:
  if (ctx) {
    delete ctx;
  }
  agent_tool_registry_destroy(r);
  return st;
}

void toolset_host_destroy(agent_tool_executor_t* executor) {
  if (!executor) {
    return;
  }
  if (executor->ctx) {
    HostToolCtx* ctx = (HostToolCtx*)executor->ctx;
    delete ctx;
  }
  executor->ctx = nullptr;
  executor->execute = nullptr;
}
