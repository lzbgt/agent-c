#include "toolset_host.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <chrono>
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
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

#if defined(AGENT_HAVE_JSONCPP)
static bool parse_json(const char* arguments_json, Json::Value* out, std::string* out_err) {
  if (out_err) {
    out_err->clear();
  }
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(arguments_json ? arguments_json : "");
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) {
      *out_err = "invalid JSON: " + errs;
    }
    return false;
  }
  *out = v;
  return true;
}
#endif

static agent_status_t set_result(agent_string_t* out, const std::string& s) {
  return agent_string_set_copy(out, s.c_str(), s.size());
}

	struct HostToolCtx {
	  std::filesystem::path root;
	  bool unrestricted = false;
	  HostCancelCallback should_cancel = nullptr;
	  void* should_cancel_ctx = nullptr;
	};

	static bool is_cancelled(const HostToolCtx* ctx) {
	  return ctx && ctx->should_cancel && ctx->should_cancel(ctx->should_cancel_ctx);
	}

static std::string to_generic_string(const std::filesystem::path& p) {
  // Prefer a stable "/"-separated form for JSON/tool output.
  return p.generic_string();
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

static std::optional<std::filesystem::path> resolve_under_root(
  const std::filesystem::path& root,
  const std::string& user_path,
  bool unrestricted
) {
  std::filesystem::path p(user_path);
  if (!unrestricted && p.is_absolute()) {
    return std::nullopt;
  }
  std::filesystem::path resolved = p.is_absolute() ? p : (root / p);
  resolved = resolved.lexically_normal();
  if (!unrestricted) {
    const std::filesystem::path norm_root = root.lexically_normal();
    if (!path_is_within(norm_root, resolved)) {
      return std::nullopt;
    }
  }
  return resolved;
}

static int64_t file_time_to_unix_ms(std::filesystem::file_time_type ft) {
  using file_clock = std::filesystem::file_time_type::clock;
  using sys_clock = std::chrono::system_clock;
  const auto sctp = std::chrono::time_point_cast<sys_clock::duration>(ft - file_clock::now() + sys_clock::now());
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();
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
  const std::string path = args["path"].asString();
  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
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
    if (data.isMember("is_binary")) {
      oss << "is_binary: " << (data["is_binary"].asBool() ? "true" : "false") << "\n";
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

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_directory(*resolved, ec)) {
    return write_envelope(false, "not a directory", Json::Value(Json::objectValue));
  }

  std::vector<std::string> exclude_names;
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

  Json::Value arr(Json::arrayValue);
  int count = 0;
  int excluded_entries = 0;
  int excluded_dirs = 0;
  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
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
  data["excluded_entries"] = excluded_entries;
  data["excluded_dirs"] = excluded_dirs;
  {
    Json::Value ex(Json::arrayValue);
    for (const auto& s : exclude_names) ex.append(s);
    data["exclude_names"] = ex;
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
    if (excluded_entries > 0) {
      oss << "excluded_entries: " << excluded_entries << "\n";
      oss << "excluded_dirs: " << excluded_dirs << "\n";
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
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec)) {
    return write_envelope(false, "path does not exist", Json::Value(Json::objectValue));
  }

  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
    if (!include_hidden && !name.empty() && name[0] == '.') return true;
    if (!exclude_names.empty() && !name.empty()) {
      for (const auto& ex : exclude_names) {
        if (name == ex) return true;
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
    if (!ec) consider(de);
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
  data["type"] = type;
  if (!name_substring.empty()) data["name_substring"] = name_substring;
  data["entries"] = entries;
  data["truncated"] = truncated;
  data["dirs_skipped"] = dirs_skipped;
  data["files_seen"] = files_seen;
  data["files_skipped_by_ext"] = files_skipped_by_ext;
  if (!extensions.empty()) {
    Json::Value exts(Json::arrayValue);
    for (const auto& s : extensions) exts.append(s);
    data["extensions"] = exts;
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

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
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

  const auto should_skip = [&](const std::filesystem::path& p) -> bool {
    const auto name = p.filename().string();
    if (!include_hidden && !name.empty() && name[0] == '.') return true;
    if (!exclude_names.empty() && !name.empty()) {
      for (const auto& ex : exclude_names) {
        if (name == ex) return true;
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
      scan_file(p);
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
  data["max_results"] = max_results;
  data["max_file_bytes"] = max_file_bytes;
  data["max_line_chars"] = max_line_chars;
  data["files_scanned"] = files_scanned;
  data["files_skipped_binary"] = files_skipped_binary;
  data["files_skipped_too_large"] = files_skipped_too_large;
  data["files_skipped_by_ext"] = files_skipped_by_ext;
  data["dirs_skipped"] = dirs_skipped;
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
    oss << "use_default_excludes: " << (use_default_excludes ? "true" : "false") << "\n";
    oss << "matches: " << (unsigned long long)matches.size() << (truncated ? " (truncated)\n" : "\n");
    oss << "files_scanned: " << files_scanned << "\n";
    if (files_skipped_binary) oss << "files_skipped_binary: " << files_skipped_binary << "\n";
    if (files_skipped_too_large) oss << "files_skipped_too_large: " << files_skipped_too_large << "\n";
    if (files_skipped_by_ext) oss << "files_skipped_by_ext: " << files_skipped_by_ext << "\n";
    if (dirs_skipped) oss << "dirs_skipped: " << dirs_skipped << "\n";
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

struct ExecResult {
  int exit_code = -1;
  bool timed_out = false;
  bool truncated = false;
  bool cancelled = false;
  std::string output;
};

static std::optional<std::filesystem::path> write_temp_file(const std::string& content, std::string* out_error) {
  std::string tmpl = (std::filesystem::temp_directory_path() / "agent_patch_XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const int fd = mkstemp(buf.data());
  if (fd < 0) {
    if (out_error) *out_error = std::string("mkstemp failed: ") + std::strerror(errno);
    return std::nullopt;
  }

  const char* p = content.data();
  size_t remaining = content.size();
  while (remaining > 0) {
    const ssize_t n = write(fd, p, remaining);
    if (n <= 0) {
      if (out_error) *out_error = std::string("write failed: ") + std::strerror(errno);
      close(fd);
      unlink(buf.data());
      return std::nullopt;
    }
    p += (size_t)n;
    remaining -= (size_t)n;
  }
  close(fd);
  return std::filesystem::path(std::string(buf.data()));
}

static ExecResult run_shell_exec(
  const std::string& cmd,
  int timeout_ms,
  size_t max_output_bytes,
  HostCancelCallback should_cancel,
  void* should_cancel_ctx
) {
  ExecResult r;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.output = "pipe failed";
    return r;
  }
  // Non-blocking read end.
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // Child: stdout/stderr -> pipe write end
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);

  pid_t pid = 0;
  const char* argv[] = {"/bin/sh", "-lc", cmd.c_str(), nullptr};
  int spawn_rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, (char* const*)argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]); // parent closes write end

  if (spawn_rc != 0) {
    close(pipefd[0]);
    r.output = std::string("posix_spawn failed: ") + std::strerror(spawn_rc);
    return r;
  }

  const auto start = std::chrono::steady_clock::now();
  bool child_done = false;
  int status = 0;

  while (true) {
    if (should_cancel && should_cancel(should_cancel_ctx)) {
      r.cancelled = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }
    // Drain output.
    char buf[4096];
    while (r.output.size() < max_output_bytes) {
      ssize_t n = read(pipefd[0], buf, sizeof(buf));
      if (n > 0) {
        size_t to_add = (size_t)n;
        if (r.output.size() + to_add > max_output_bytes) {
          to_add = max_output_bytes - r.output.size();
        }
        r.output.append(buf, buf + to_add);
        if (to_add < (size_t)n) {
          // Truncated.
          r.truncated = true;
          break;
        }
      } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        break;
      }
    }

    if (!child_done) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        child_done = true;
      }
    }

    if (child_done) {
      break;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (timeout_ms > 0 && elapsed > timeout_ms) {
      r.timed_out = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }

    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    (void)poll(&pfd, 1, 50);
  }

  close(pipefd[0]);

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  } else {
    r.exit_code = -1;
  }

  return r;
}

static agent_status_t tool_shell_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"shell_exec requires jsoncpp\"}");
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
  if (!args["cmd"].isString()) {
    return write_envelope(false, "missing string field 'cmd'", Json::Value(Json::objectValue));
  }
  const std::string cmd = args["cmd"].asString();
  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;

  ExecResult r = run_shell_exec(cmd, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);

  Json::Value data(Json::objectValue);
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["cancelled"] = r.cancelled;
  data["output"] = r.output;
  // Note: ok is a hint only; the model should still judge based on output for cases like pip warnings.
  const bool ok_hint = (!r.timed_out && !r.cancelled && r.exit_code == 0);
  return write_envelope(ok_hint, r.cancelled ? "cancelled" : "", data);
#endif
}

static ExecResult run_proc_exec(
  const std::vector<std::string>& argv,
  int timeout_ms,
  size_t max_output_bytes,
  HostCancelCallback should_cancel,
  void* should_cancel_ctx
) {
  ExecResult r;
  if (argv.empty()) {
    r.output = "argv is empty";
    return r;
  }

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.output = "pipe failed";
    return r;
  }
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& s : argv) {
    cargv.push_back(const_cast<char*>(s.c_str()));
  }
  cargv.push_back(nullptr);

  pid_t pid = 0;
  int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);

  if (spawn_rc != 0) {
    close(pipefd[0]);
    r.output = std::string("posix_spawnp failed: ") + std::strerror(spawn_rc);
    return r;
  }

  const auto start = std::chrono::steady_clock::now();
  bool child_done = false;
  int status = 0;

  while (true) {
    if (should_cancel && should_cancel(should_cancel_ctx)) {
      r.cancelled = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }
    char buf[4096];
    while (r.output.size() < max_output_bytes) {
      ssize_t n = read(pipefd[0], buf, sizeof(buf));
      if (n > 0) {
        size_t to_add = (size_t)n;
        if (r.output.size() + to_add > max_output_bytes) {
          to_add = max_output_bytes - r.output.size();
        }
        r.output.append(buf, buf + to_add);
        if (to_add < (size_t)n) {
          r.truncated = true;
          break;
        }
      } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        break;
      }
    }

    if (!child_done) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        child_done = true;
      }
    }

    if (child_done) {
      break;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (timeout_ms > 0 && elapsed > timeout_ms) {
      r.timed_out = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }

    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    (void)poll(&pfd, 1, 50);
  }

  close(pipefd[0]);

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  } else {
    r.exit_code = -1;
  }

  return r;
}

static agent_status_t tool_proc_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"proc_exec requires jsoncpp\"}");
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
  if (!args["argv"].isArray()) {
    return write_envelope(false, "missing array field 'argv'", Json::Value(Json::objectValue));
  }
  std::vector<std::string> argv;
  for (const auto& v : args["argv"]) {
    if (!v.isString()) {
      continue;
    }
    argv.push_back(v.asString());
  }
  if (argv.empty()) {
    return write_envelope(false, "argv is empty", Json::Value(Json::objectValue));
  }
  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;

  ExecResult r = run_proc_exec(argv, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);
  Json::Value data(Json::objectValue);
  data["argv"] = Json::Value(Json::arrayValue);
  for (const auto& s : argv) {
    data["argv"].append(s);
  }
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["cancelled"] = r.cancelled;
  data["output"] = r.output;
  const bool ok_hint = (!r.timed_out && !r.cancelled && r.exit_code == 0);
  return write_envelope(ok_hint, r.cancelled ? "cancelled" : "", data);
#endif
}

static agent_status_t tool_file_apply_patch(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"file_apply_patch requires jsoncpp\"}");
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
  if (!args["patch"].isString()) {
    return write_envelope(false, "missing string field 'patch'", Json::Value(Json::objectValue));
  }

  const std::string patch = args["patch"].asString();
  if (patch.empty()) {
    return write_envelope(false, "patch is empty", Json::Value(Json::objectValue));
  }

  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;
  const bool unsafe_paths = args.isMember("unsafe_paths") && args["unsafe_paths"].isBool() ? args["unsafe_paths"].asBool() : ctx->unrestricted;

  std::string tmp_err;
  const std::optional<std::filesystem::path> tmp = write_temp_file(patch, &tmp_err);
  if (!tmp) {
    return write_envelope(false, tmp_err.empty() ? "failed to write temp patch" : tmp_err, Json::Value(Json::objectValue));
  }

  auto run_git_apply = [&](const std::vector<std::string>& extra_args) -> ExecResult {
    std::vector<std::string> argv;
    argv.push_back("git");
    if (!ctx->unrestricted) {
      argv.push_back("-C");
      argv.push_back(ctx->root.string());
    }
    argv.push_back("apply");
    if (unsafe_paths) {
      argv.push_back("--unsafe-paths");
    }
    for (const auto& a : extra_args) {
      argv.push_back(a);
    }
    argv.push_back(tmp->string());
    return run_proc_exec(argv, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);
  };

  ExecResult check = run_git_apply({"--check"});
  ExecResult apply;
  bool ok = (!check.timed_out && check.exit_code == 0);
  if (ok) {
    apply = run_git_apply({});
    ok = (!apply.timed_out && apply.exit_code == 0);
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "git apply";
  data["root_dir"] = ctx->unrestricted ? Json::Value("") : Json::Value(ctx->root.string());
  data["unsafe_paths"] = unsafe_paths;
  data["patch"] = patch;
  data["check"] = Json::Value(Json::objectValue);
  data["check"]["exit_code"] = check.exit_code;
  data["check"]["timed_out"] = check.timed_out;
  data["check"]["truncated"] = check.truncated;
  data["check"]["output"] = check.output;
  if (ok) {
    data["apply"] = Json::Value(Json::objectValue);
    data["apply"]["exit_code"] = apply.exit_code;
    data["apply"]["timed_out"] = apply.timed_out;
    data["apply"]["truncated"] = apply.truncated;
    data["apply"]["output"] = apply.output;
  }

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);
  return write_envelope(ok, ok ? "" : "git apply failed", data);
#endif
}

static agent_status_t host_tools_execute(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!vctx || !tool_name || !arguments_json || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  HostToolCtx* ctx = (HostToolCtx*)vctx;
  const std::string name(tool_name);
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

  // Process exec (primary mechanism for host-side tooling).
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
    "  \"unsafe_paths\":{\"type\":\"boolean\"}"
    "},"
    "\"required\":[\"patch\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  // Read-only filesystem tools (host-only):
  // These exist to reduce token waste by bounding outputs and supporting pagination.
  st = add_tool(
    r,
    "fs_stat",
    "Stat a file or directory (host-side). Returns JSON envelope with metadata and a human-readable `data.output`.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"File/directory path (relative to tools root unless yolo/unrestricted).\"}"
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
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"}"
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
    "  \"type\":{\"type\":\"string\",\"description\":\"Entry type filter: any|file|dir (default: any).\"},"
    "  \"name_substring\":{\"type\":\"string\",\"description\":\"Optional substring filter on basename.\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"}"
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
    "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max matches to return (default: 200).\"},"
    "  \"max_file_bytes\":{\"type\":\"integer\",\"description\":\"Skip files larger than this many bytes (default: 524288). 0 disables size limit.\"},"
    "  \"max_line_chars\":{\"type\":\"integer\",\"description\":\"Max chars per snippet line (default: 400).\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"}"
    "},"
    "\"required\":[\"query\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  // Executor context (owned by host; for CLI we just heap-allocate).
  ctx = new (std::nothrow) HostToolCtx();
  if (!ctx) {
    st = AGENT_ERR_OOM;
    goto fail;
  }
  ctx->unrestricted = cfg.root_dir.empty();
  ctx->root = cfg.root_dir.empty() ? std::filesystem::current_path()
                                   : std::filesystem::path(cfg.root_dir);
  ctx->should_cancel = cfg.should_cancel;
  ctx->should_cancel_ctx = cfg.should_cancel_ctx;
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
