#include "toolset_host_internal.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace host_tools_internal {

#if !defined(AGENT_HAVE_JSONCPP)
agent_status_t tool_memory_write(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_write requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_get(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_get requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_search(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_search requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_put(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_put requires jsoncpp\",\"data\":{}}");
}
#else

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static std::string trim_ascii(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static bool is_safe_relpath_md(const std::string& p) {
  if (p.empty()) return false;
  if (p.size() > 300) return false;
  if (p.find('\\') != std::string::npos) return false;
  if (p[0] == '/') return false;
  if (p.find("..") != std::string::npos) return false;
  if (p.find('\0') != std::string::npos) return false;
  const std::string lp = to_lower_ascii(p);
  if (lp.size() < 3 || lp.rfind(".md") != lp.size() - 3) return false;
  return true;
}

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string iso_utc_now() {
  const auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(
    buf,
    sizeof(buf),
    "%04d-%02d-%02dT%02d:%02d:%02dZ",
    tm.tm_year + 1900,
    tm.tm_mon + 1,
    tm.tm_mday,
    tm.tm_hour,
    tm.tm_min,
    tm.tm_sec
  );
  return std::string(buf);
}

static std::string local_date_ymd() {
  const auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

static std::filesystem::path memory_root_from_ctx(const HostToolCtx* ctx) {
  if (!ctx) return {};
  if (ctx->sessions_root_dir.empty()) return {};
  // Historical layout: sessions_root_dir = <state_dir>/sessions
  // Current default layout: sessions_root_dir = <state_dir>
  const std::filesystem::path sr = ctx->sessions_root_dir;
  const std::filesystem::path state_dir = (sr.filename() == "sessions") ? sr.parent_path() : sr;
  if (state_dir.empty()) return {};
  return state_dir / "memory";
}

static std::filesystem::path memory_file_for_layer(
  const HostToolCtx* ctx,
  const std::string& layer,
  const std::string& rel_path_opt
) {
  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) return {};

  const std::string l = to_lower_ascii(trim_ascii(layer));
  if (!rel_path_opt.empty()) {
    if (!is_safe_relpath_md(rel_path_opt)) return {};
    return (mem_root / rel_path_opt).lexically_normal();
  }

  if (l.empty() || l == "daily") {
    return mem_root / (local_date_ymd() + ".md");
  }
  if (l == "core") {
    return mem_root / "MEMORY.md";
  }
  if (l == "session") {
    const std::string sid = trim_ascii(ctx ? ctx->session_id : "");
    if (sid.empty()) return {};
    return mem_root / "sessions" / (sid + ".md");
  }
  return {};
}

static agent_status_t write_envelope(agent_string_t* out_result, bool ok, const std::string& error, const Json::Value& data) {
  Json::Value o(Json::objectValue);
  o["ok"] = ok;
  if (!error.empty()) o["error"] = error;
  o["data"] = data;
  return set_result(out_result, json_stringify_compact(o));
}

agent_status_t tool_memory_write(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  if (ctx->policy != HostToolsetPolicyMode::Full) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_write";
    d["policy"] = "readonly";
    return write_envelope(out_result, false, "tool disabled by policy", d);
  }

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }
  const std::string text = args.isMember("text") && args["text"].isString() ? args["text"].asString() : "";
  if (trim_ascii(text).empty()) {
    return write_envelope(out_result, false, "missing string field 'text'", Json::Value(Json::objectValue));
  }

  const std::string layer = args.isMember("layer") && args["layer"].isString() ? args["layer"].asString() : "daily";
  const std::string rel_path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";
  const std::filesystem::path out_path = memory_file_for_layer(ctx, layer, rel_path);
  if (out_path.empty()) {
    return write_envelope(out_result, false, "memory_write requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  std::error_code ec;
  std::filesystem::create_directories(out_path.parent_path(), ec);
  if (ec) {
    return write_envelope(out_result, false, "failed to create memory directory", Json::Value(Json::objectValue));
  }

  const std::string title = args.isMember("title") && args["title"].isString() ? trim_ascii(args["title"].asString()) : "";
  const bool with_heading = args.isMember("with_heading") && args["with_heading"].isBool() ? args["with_heading"].asBool() : true;

  std::string entry;
  if (with_heading) {
    entry += "\n\n### ";
    entry += iso_utc_now();
    if (!title.empty()) {
      entry += " — ";
      entry += title;
    }
    entry += "\n";
  } else {
    entry += "\n\n";
  }
  entry += text;
  if (!entry.empty() && entry.back() != '\n') entry.push_back('\n');

  {
    std::ofstream out(out_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
      return write_envelope(out_result, false, "failed to open memory file for append", Json::Value(Json::objectValue));
    }
    out.write(entry.data(), (std::streamsize)entry.size());
    if (!out.good()) {
      return write_envelope(out_result, false, "failed to write memory entry", Json::Value(Json::objectValue));
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_write";
  data["layer"] = layer;
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = to_generic_string(out_path.lexically_relative(mem_root));
  data["abs_path"] = to_generic_string(out_path);
  data["bytes_appended"] = (Json::Int64)entry.size();
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  data["output"] = "memory_write: appended to " + data["path"].asString();
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_get(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }

  const std::string rel_path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";
  if (!is_safe_relpath_md(rel_path)) {
    return write_envelope(out_result, false, "missing/invalid string field 'path' (must be a safe relative .md path)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_get requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }

  std::ifstream in(abs, std::ios::binary);
  if (!in.is_open()) {
    return write_envelope(out_result, false, "file not found", Json::Value(Json::objectValue));
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();

  const int from = args.isMember("from_line") && args["from_line"].isInt() ? std::max(1, args["from_line"].asInt()) : 1;
  const int max_lines = args.isMember("max_lines") && args["max_lines"].isInt() ? std::max(1, args["max_lines"].asInt()) : 200;

  std::string out_text;
  if (from == 1 && max_lines >= 20000) {
    out_text = content;
  } else {
    std::vector<std::string> lines;
    {
      std::istringstream iss(content);
      std::string line;
      while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
      }
    }
    const int start = std::min<int>(from, (int)lines.size() + 1);
    const int end = std::min<int>(start - 1 + max_lines, (int)lines.size());
    std::ostringstream oss;
    for (int i = start; i <= end; i++) {
      if (i > start) oss << "\n";
      oss << lines[(size_t)i - 1];
    }
    out_text = oss.str();
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_get";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["from_line"] = from;
  data["max_lines"] = max_lines;
  data["text"] = out_text;
  data["output"] = "memory_get: " + rel_path;
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_put(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  if (ctx->policy != HostToolsetPolicyMode::Full) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_put";
    d["policy"] = "readonly";
    return write_envelope(out_result, false, "tool disabled by policy", d);
  }

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }
  const std::string rel_path = args.isMember("path") && args["path"].isString() ? trim_ascii(args["path"].asString()) : "";
  if (!is_safe_relpath_md(rel_path)) {
    return write_envelope(out_result, false, "missing/invalid string field 'path' (must be a safe relative .md path)", Json::Value(Json::objectValue));
  }
  const std::string text = args.isMember("text") && args["text"].isString() ? args["text"].asString() : "";
  // Allow empty text (truncate file to empty), but require the field.
  if (!args.isMember("text") || !args["text"].isString()) {
    return write_envelope(out_result, false, "missing string field 'text'", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_put requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }
  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  std::filesystem::create_directories(abs.parent_path(), ec);
  if (ec) {
    return write_envelope(out_result, false, "failed to create memory directory", Json::Value(Json::objectValue));
  }

  std::string out_text = text;
  if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');

  {
    std::ofstream out(abs, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return write_envelope(out_result, false, "failed to open memory file for write", Json::Value(Json::objectValue));
    }
    out.write(out_text.data(), (std::streamsize)out_text.size());
    if (!out.good()) {
      return write_envelope(out_result, false, "failed to write memory file", Json::Value(Json::objectValue));
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_put";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["bytes_written"] = (Json::Int64)out_text.size();
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  data["output"] = "memory_put: wrote " + rel_path;
  return write_envelope(out_result, true, "", data);
}

static std::vector<std::string> list_candidate_memory_files(const HostToolCtx* ctx, int max_days) {
  std::vector<std::string> out;
  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) return out;

  const std::filesystem::path core = mem_root / "MEMORY.md";
  std::error_code ec;
  if (std::filesystem::exists(core, ec) && std::filesystem::is_regular_file(core, ec)) {
    out.push_back(to_generic_string(core));
  }

  if (max_days <= 0) return out;

  // Best-effort: scan recent daily files only (bounded). This avoids unbounded directory walks.
  // We follow OpenClaw's convention: memory/YYYY-MM-DD.md, but here daily files live directly under mem_root.
  // (mem_root is already a "memory" directory in agentd state).
  const auto now = std::chrono::system_clock::now();
  for (int i = 0; i < max_days; i++) {
    const auto day = now - std::chrono::hours(24 * i);
    std::time_t t = std::chrono::system_clock::to_time_t(day);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d.md", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    const std::filesystem::path p = mem_root / buf;
    ec.clear();
    if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
      out.push_back(to_generic_string(p));
    }
  }
  return out;
}

agent_status_t tool_memory_search(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }

  const std::string query = args.isMember("query") && args["query"].isString() ? trim_ascii(args["query"].asString()) : "";
  if (query.empty()) {
    return write_envelope(out_result, false, "missing string field 'query'", Json::Value(Json::objectValue));
  }
  const int max_results = args.isMember("max_results") && args["max_results"].isInt() ? std::max(1, args["max_results"].asInt()) : 20;
  const int max_days = args.isMember("daily_days") && args["daily_days"].isInt() ? std::max(0, args["daily_days"].asInt()) : 14;
  const bool case_sensitive =
    args.isMember("case_sensitive") && args["case_sensitive"].isBool() ? args["case_sensitive"].asBool() : false;
  const int context_lines = args.isMember("context_lines") && args["context_lines"].isInt() ? std::max(0, args["context_lines"].asInt()) : 2;
  const int max_snippet_chars = args.isMember("max_snippet_chars") && args["max_snippet_chars"].isInt()
    ? std::max(80, args["max_snippet_chars"].asInt())
    : 600;

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_search requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::vector<std::string> files = list_candidate_memory_files(ctx, max_days);
  const std::string q = case_sensitive ? query : to_lower_ascii(query);

  Json::Value results(Json::arrayValue);

  for (const auto& abs_str : files) {
    if ((int)results.size() >= max_results) break;
    std::filesystem::path abs(abs_str);
    std::ifstream in(abs, std::ios::binary);
    if (!in.is_open()) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    if (content.size() > 1024 * 1024) {
      content.resize(1024 * 1024);
    }
    std::vector<std::string> lines;
    {
      std::istringstream iss(content);
      std::string line;
      while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (lines.size() > 20000) break;
      }
    }
    for (size_t i = 0; i < lines.size(); i++) {
      if ((int)results.size() >= max_results) break;
      const std::string hay = case_sensitive ? lines[i] : to_lower_ascii(lines[i]);
      if (hay.find(q) == std::string::npos) continue;

      const int line_no = (int)i + 1;
      const int lo = std::max(1, line_no - context_lines);
      const int hi = std::min((int)lines.size(), line_no + context_lines);
      std::ostringstream snip;
      for (int ln = lo; ln <= hi; ln++) {
        if (ln > lo) snip << "\n";
        snip << lines[(size_t)ln - 1];
      }
      std::string snippet = snip.str();
      if ((int)snippet.size() > max_snippet_chars) {
        snippet.resize((size_t)max_snippet_chars);
      }

      std::error_code ec;
      std::filesystem::path rel = std::filesystem::relative(abs, mem_root, ec);
      if (ec) rel = abs.lexically_relative(mem_root);
      const std::string rel_s = to_generic_string(rel.lexically_normal());

      Json::Value r(Json::objectValue);
      r["path"] = rel_s;
      r["line"] = line_no;
      r["snippet"] = snippet;
      results.append(r);
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_search";
  data["memory_root"] = to_generic_string(mem_root);
  data["query"] = query;
  data["files_scanned"] = (Json::Int64)files.size();
  data["results"] = results;
  data["output"] = "memory_search results=" + std::to_string((unsigned)results.size());
  return write_envelope(out_result, true, "", data);
}

#endif  // AGENT_HAVE_JSONCPP

} // namespace host_tools_internal
