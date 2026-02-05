#include "toolset_host_internal.h"

#include "memory_index.h"
#include "agent_sha256.h"

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
agent_status_t tool_memory_structured_query(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_structured_query requires jsoncpp\",\"data\":{}}");
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

static bool eq_ci_ascii(const std::string& a, const std::string& b) {
  return to_lower_ascii(a) == to_lower_ascii(b);
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

static const char* kAgentMemoryV1Begin = "<!-- AGENT_MEMORY_V1_BEGIN -->";
static const char* kAgentMemoryV1End = "<!-- AGENT_MEMORY_V1_END -->";
static const char* kAgentMemoryV1NotesBegin = "<!-- AGENT_MEMORY_V1_NOTES_BEGIN -->";
static const char* kAgentMemoryV1NotesEnd = "<!-- AGENT_MEMORY_V1_NOTES_END -->";

static std::string extract_block_body(const std::string& s, const std::string& begin, const std::string& end) {
  const size_t a = s.find(begin);
  if (a == std::string::npos) return "";
  const size_t b = s.find(end, a + begin.size());
  if (b == std::string::npos) return "";
  const size_t body_a = a + begin.size();
  const size_t body_b = b;
  if (body_b <= body_a) return "";
  return s.substr(body_a, body_b - body_a);
}

static bool parse_json_object_str(const std::string& s, Json::Value* out_obj, std::string* out_err) {
  if (out_err) out_err->clear();
  if (out_obj) *out_obj = Json::Value(Json::objectValue);
  if (!out_obj) return false;

  std::string errs;
  Json::CharReaderBuilder rb;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) {
    if (out_err) *out_err = errs.empty() ? "invalid json" : errs;
    return false;
  }
  *out_obj = v;
  return true;
}

static bool json_array_contains_string_ci(const Json::Value& arr, const std::string& s) {
  if (!arr.isArray()) return false;
  const std::string t = to_lower_ascii(trim_ascii(s));
  if (t.empty()) return false;
  for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
    if (!arr[i].isString()) continue;
    if (to_lower_ascii(trim_ascii(arr[i].asString())) == t) return true;
  }
  return false;
}

static void json_array_append_unique_string_ci(Json::Value* arr, const std::string& s, int max_items) {
  if (!arr) return;
  if (!arr->isArray()) *arr = Json::Value(Json::arrayValue);
  const std::string v = trim_ascii(s);
  if (v.empty()) return;
  if (json_array_contains_string_ci(*arr, v)) return;
  arr->append(v);
  // Keep last N items (drop oldest).
  while (max_items > 0 && (int)arr->size() > max_items) {
    arr->removeIndex(0, nullptr);
  }
}

static std::string markdown_escape_inline(const std::string& s) {
  // Keep this minimal; we only render bullet summaries.
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\n' || c == '\r') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

static std::string render_structured_memory_markdown(const Json::Value& doc) {
  // doc: { schema, items: { key: {kind,value,status,updated_utc,...} } }
  std::vector<std::string> fact_keys;
  std::vector<std::string> pref_keys;
  std::vector<std::string> task_keys;
  std::vector<std::string> dep_keys;

  const auto& items = doc["items"];
  if (items.isObject()) {
    for (const auto& key : items.getMemberNames()) {
      const auto& it = items[key];
      if (!it.isObject()) continue;
      const std::string status = it.isMember("status") && it["status"].isString() ? to_lower_ascii(it["status"].asString()) : "active";
      if (status == "deprecated") {
        dep_keys.push_back(key);
        continue;
      }
      const std::string kind = it.isMember("kind") && it["kind"].isString() ? to_lower_ascii(it["kind"].asString()) : "fact";
      if (kind == "preference" || kind == "pref") pref_keys.push_back(key);
      else if (kind == "task") task_keys.push_back(key);
      else fact_keys.push_back(key);
    }
  }
  auto sort_keys = [](std::vector<std::string>& v) { std::sort(v.begin(), v.end()); };
  sort_keys(fact_keys);
  sort_keys(pref_keys);
  sort_keys(task_keys);
  sort_keys(dep_keys);

  auto section = [&](const char* title, const std::vector<std::string>& keys) -> std::string {
    std::ostringstream oss;
    oss << "## " << (title ? title : "") << "\n";
    if (keys.empty()) {
      oss << "_(empty)_\n";
      return oss.str();
    }
    for (const auto& k : keys) {
      const auto& it = items[k];
      const std::string value = it.isMember("value") && it["value"].isString() ? it["value"].asString() : "";
      const std::string updated = it.isMember("updated_utc") && it["updated_utc"].isString() ? it["updated_utc"].asString() : "";
      const std::string observed = it.isMember("observed_utc") && it["observed_utc"].isString() ? it["observed_utc"].asString() : "";
      const int sources_n = it.isMember("sources") && it["sources"].isArray() ? (int)it["sources"].size() : 0;
      oss << "- **" << markdown_escape_inline(k) << "**: " << markdown_escape_inline(value);
      if (!updated.empty()) oss << " _(updated " << markdown_escape_inline(updated) << ")_";
      if (!observed.empty() && observed != updated) oss << " _(observed " << markdown_escape_inline(observed) << ")_";
      if (sources_n > 0) oss << " _(sources " << sources_n << ")_";
      oss << "\n";
    }
    return oss.str();
  };

  std::ostringstream out;
  out << "# Structured Memory\n\n";
  out << "This file is machine-maintained by `memory_put` (structured mode).\n";
  out << "Edit via tools to avoid merge/conflict issues.\n\n";
  out << section("Facts", fact_keys) << "\n";
  out << section("Preferences", pref_keys) << "\n";
  out << section("Tasks", task_keys) << "\n";
  out << section("Deprecated", dep_keys) << "\n";
  return out.str();
}

static std::string build_structured_memory_file_text(const Json::Value& doc, const std::string& user_notes_md) {
  std::ostringstream oss;
  oss << render_structured_memory_markdown(doc);
  oss << "\n";
  oss << kAgentMemoryV1Begin << "\n";
  oss << json_stringify_compact(doc) << "\n";
  oss << kAgentMemoryV1End << "\n";
  oss << "\n";
  oss << kAgentMemoryV1NotesBegin << "\n";
  if (!trim_ascii(user_notes_md).empty()) {
    oss << user_notes_md;
    if (!user_notes_md.empty() && user_notes_md.back() != '\n') oss << "\n";
  } else {
    oss << "_(optional freeform notes; preserved across structured updates)_\n";
  }
  oss << kAgentMemoryV1NotesEnd << "\n";
  return oss.str();
}

static bool parse_structured_memory_doc(const std::string& file_text, Json::Value* out_doc, std::string* out_err) {
  if (out_err) out_err->clear();
  if (out_doc) *out_doc = Json::Value(Json::objectValue);
  if (!out_doc) return false;

  const std::string body = extract_block_body(file_text, kAgentMemoryV1Begin, kAgentMemoryV1End);
  if (trim_ascii(body).empty()) {
    // Initialize empty doc.
    Json::Value doc(Json::objectValue);
    doc["schema"] = "agent_memory_v2";
    doc["items"] = Json::Value(Json::objectValue);
    *out_doc = doc;
    return true;
  }
  Json::Value doc;
  std::string perr;
  if (!parse_json_object_str(body, &doc, &perr)) {
    if (out_err) *out_err = perr.empty() ? "failed to parse structured memory json" : perr;
    return false;
  }
  if (!doc.isMember("schema") || !doc["schema"].isString()) doc["schema"] = "agent_memory_v2";
  if (!doc.isMember("items") || !doc["items"].isObject()) doc["items"] = Json::Value(Json::objectValue);

  // Best-effort upgrade to v2 fields for deterministic versioning/evidence merge.
  // Keep markers as V1 for backwards compatibility (only schema + payload evolve).
  const std::string schema = doc["schema"].isString() ? doc["schema"].asString() : "agent_memory_v2";
  if (schema != "agent_memory_v2") {
    doc["schema"] = "agent_memory_v2";
  }
  Json::Value& items = doc["items"];
  for (const auto& key : items.getMemberNames()) {
    Json::Value& it = items[key];
    if (!it.isObject()) continue;
    if (!it.isMember("versions") || !it["versions"].isArray()) it["versions"] = Json::Value(Json::arrayValue);
    if (!it.isMember("sources") || !it["sources"].isArray()) {
      it["sources"] = Json::Value(Json::arrayValue);
      if (it.isMember("source") && it["source"].isString() && !trim_ascii(it["source"].asString()).empty()) {
        it["sources"].append(trim_ascii(it["source"].asString()));
      }
    }
    if (!it.isMember("observed_utc") || !it["observed_utc"].isString()) {
      if (it.isMember("updated_utc") && it["updated_utc"].isString()) it["observed_utc"] = it["updated_utc"];
      else it["observed_utc"] = iso_utc_now();
    }
  }

  *out_doc = doc;
  return true;
}

static std::string extract_user_notes_md(const std::string& file_text) {
  const std::string notes = extract_block_body(file_text, kAgentMemoryV1NotesBegin, kAgentMemoryV1NotesEnd);
  return trim_ascii(notes).empty() ? "" : notes;
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
  const bool structured = args.isMember("entries") && args["entries"].isArray();
  const std::string text = (!structured && args.isMember("text") && args["text"].isString()) ? args["text"].asString() : "";
  // Legacy mode: allow empty text (truncate file to empty), but require the field.
  if (!structured && (!args.isMember("text") || !args["text"].isString())) {
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

  std::string out_text;
  Json::Value structured_doc_for_checkpoint(Json::objectValue);
  bool structured_doc_present = false;
  int structured_entries_valid = 0;
  int structured_entries_changed = 0;
  int structured_entries_superseded = 0;
  int structured_entries_evidence_added = 0;
  if (structured) {
    // Structured upsert mode: deterministic key conflict resolution for durable facts/preferences/tasks.
    std::string existing;
    {
      std::ifstream in(abs, std::ios::binary);
      if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        existing = ss.str();
      }
    }

    Json::Value doc;
    std::string derr;
    if (!parse_structured_memory_doc(existing, &doc, &derr)) {
      return write_envelope(out_result, false, std::string("failed to parse structured memory doc: ") + derr, Json::Value(Json::objectValue));
    }

    const std::string user_notes = extract_user_notes_md(existing);
    const auto& entries = args["entries"];
    if (!entries.isArray() || entries.empty()) {
      return write_envelope(out_result, false, "entries must be a non-empty array", Json::Value(Json::objectValue));
    }

    Json::Value& items = doc["items"];
    if (!items.isObject()) items = Json::Value(Json::objectValue);
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      const auto& e = entries[i];
      if (!e.isObject()) continue;
      const std::string key = e.isMember("key") && e["key"].isString() ? trim_ascii(e["key"].asString()) : "";
      const std::string kind = e.isMember("kind") && e["kind"].isString() ? trim_ascii(e["kind"].asString()) : "fact";
      const std::string value = e.isMember("value") && e["value"].isString() ? e["value"].asString() : "";
      const std::string status = e.isMember("status") && e["status"].isString() ? trim_ascii(e["status"].asString()) : "active";
      const std::string source = e.isMember("source") && e["source"].isString() ? trim_ascii(e["source"].asString()) : "";
      if (key.empty() || value.empty()) continue;
      structured_entries_valid++;

      const std::string norm_kind = trim_ascii(kind).empty() ? "fact" : kind;
      const std::string norm_status = trim_ascii(status).empty() ? "active" : status;
      const std::string now_utc = iso_utc_now();

      if (!items.isMember(key) || !items[key].isObject()) {
        Json::Value rec(Json::objectValue);
        rec["kind"] = norm_kind;
        rec["value"] = value;
        rec["status"] = norm_status;
        rec["updated_utc"] = now_utc;
        rec["observed_utc"] = now_utc;
        rec["versions"] = Json::Value(Json::arrayValue);
        rec["sources"] = Json::Value(Json::arrayValue);
        if (!source.empty()) json_array_append_unique_string_ci(&rec["sources"], source, 20);
        items[key] = rec;
        structured_entries_changed++;
        continue;
      }

      Json::Value& cur = items[key];
      if (!cur.isMember("versions") || !cur["versions"].isArray()) cur["versions"] = Json::Value(Json::arrayValue);
      if (!cur.isMember("sources") || !cur["sources"].isArray()) cur["sources"] = Json::Value(Json::arrayValue);
      if (!cur.isMember("observed_utc") || !cur["observed_utc"].isString()) {
        cur["observed_utc"] = cur.isMember("updated_utc") && cur["updated_utc"].isString() ? cur["updated_utc"] : now_utc;
      }

      const std::string cur_kind = cur.isMember("kind") && cur["kind"].isString() ? cur["kind"].asString() : "fact";
      const std::string cur_value = cur.isMember("value") && cur["value"].isString() ? cur["value"].asString() : "";
      const std::string cur_status = cur.isMember("status") && cur["status"].isString() ? cur["status"].asString() : "active";

      const bool same_core = eq_ci_ascii(cur_kind, norm_kind) && cur_value == value && eq_ci_ascii(cur_status, norm_status);
      const bool need_add_source = !source.empty() && !json_array_contains_string_ci(cur["sources"], source);

      if (same_core) {
        if (!need_add_source) continue;
        json_array_append_unique_string_ci(&cur["sources"], source, 20);
        cur["observed_utc"] = now_utc;
        structured_entries_evidence_added++;
        structured_entries_changed++;
        continue;
      }

      // Supersede: move previous current to versions, then set new current.
      Json::Value prev(Json::objectValue);
      prev["kind"] = cur_kind;
      prev["value"] = cur_value;
      prev["status"] = cur_status;
      if (cur.isMember("updated_utc")) prev["updated_utc"] = cur["updated_utc"];
      if (cur.isMember("observed_utc")) prev["observed_utc"] = cur["observed_utc"];
      if (cur.isMember("sources") && cur["sources"].isArray()) prev["sources"] = cur["sources"];
      prev["superseded_utc"] = now_utc;

      Json::Value& vers = cur["versions"];
      vers.insert(0U, prev); // newest first
      while ((int)vers.size() > 20) vers.removeIndex((Json::ArrayIndex)vers.size() - 1, nullptr);

      cur["kind"] = norm_kind;
      cur["value"] = value;
      cur["status"] = norm_status;
      cur["updated_utc"] = now_utc;
      cur["observed_utc"] = now_utc;
      cur["sources"] = Json::Value(Json::arrayValue);
      if (!source.empty()) json_array_append_unique_string_ci(&cur["sources"], source, 20);
      structured_entries_superseded++;
      structured_entries_changed++;
    }
    if (structured_entries_valid == 0) {
      return write_envelope(out_result, false, "no valid entries applied (each entry requires key + value)", Json::Value(Json::objectValue));
    }
    if (structured_entries_changed == 0) {
      Json::Value data(Json::objectValue);
      data["tool"] = "memory_put";
      data["memory_root"] = to_generic_string(mem_root);
      data["path"] = rel_path;
      data["abs_path"] = to_generic_string(abs);
      data["structured"] = true;
      data["no_changes"] = true;
      data["entries_valid"] = structured_entries_valid;
      data["entries_changed"] = structured_entries_changed;
      data["entries_superseded"] = structured_entries_superseded;
      data["entries_evidence_added"] = structured_entries_evidence_added;
      data["bytes_written"] = (Json::Int64)0;
      data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
      data["output"] = "memory_put: no changes for " + rel_path;
      return write_envelope(out_result, true, "", data);
    }

    structured_doc_for_checkpoint = doc;
    structured_doc_present = true;
    out_text = build_structured_memory_file_text(doc, user_notes);
    if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');
  } else {
    out_text = text;
    if (!out_text.empty() && out_text.back() != '\n') out_text.push_back('\n');
  }

  bool wrote_file = false;
  if (structured) {
    wrote_file = structured_entries_changed > 0;
  } else {
    wrote_file = true;
  }
  if (wrote_file) {
    std::ofstream out(abs, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return write_envelope(out_result, false, "failed to open memory file for write", Json::Value(Json::objectValue));
    }
    out.write(out_text.data(), (std::streamsize)out_text.size());
    if (!out.good()) {
      return write_envelope(out_result, false, "failed to write memory file", Json::Value(Json::objectValue));
    }
  }

  // Rolling consolidation checkpoint: when a structured memory file is updated, keep a time-stamped
  // JSON snapshot for correlation over time (best-effort; does not fail the tool call).
  bool checkpoint_ok = false;
  std::string checkpoint_path_rel;
  std::string checkpoint_ts_utc;
  std::string checkpoint_sha256;
  int64_t checkpoint_bytes = 0;
  if (wrote_file && structured && structured_doc_present) {
    const bool want_checkpoint =
      args.isMember("checkpoint") && args["checkpoint"].isBool() ? args["checkpoint"].asBool() : true;
    const int keep_checkpoints =
      args.isMember("keep_checkpoints") && args["keep_checkpoints"].isInt() ? std::max(1, args["keep_checkpoints"].asInt()) : 100;
    if (want_checkpoint) {
      const std::filesystem::path ckdir = mem_root / "checkpoints";
      std::error_code ec2;
      std::filesystem::create_directories(ckdir, ec2);
      if (!ec2) {
        const std::string ts = iso_utc_now();
        checkpoint_ts_utc = ts;
        std::string safe_ts = ts;
        for (char& c : safe_ts) {
          if (c == ':' || c == '/') c = '-';
        }
        const std::filesystem::path ckfile = ckdir / (std::string("structured_") + safe_ts + ".json");

        Json::Value ck(Json::objectValue);
        ck["schema"] = "agent_memory_checkpoint_v1";
        ck["ts_utc"] = ts;
        ck["path"] = rel_path;
        ck["doc"] = structured_doc_for_checkpoint;
        const std::string ck_text = json_stringify_compact(ck) + "\n";
        checkpoint_bytes = (int64_t)ck_text.size();
        {
          char hex[65];
          agent_sha256_hex_of_bytes(ck_text.data(), ck_text.size(), hex);
          checkpoint_sha256 = std::string(hex);
        }

        {
          std::ofstream out(ckfile, std::ios::binary | std::ios::trunc);
          if (out.is_open()) {
            out.write(ck_text.data(), (std::streamsize)ck_text.size());
            checkpoint_ok = out.good();
          }
        }

        if (checkpoint_ok) {
          checkpoint_path_rel = to_generic_string(ckfile.lexically_relative(mem_root));
          // Best-effort pruning: keep only the newest N checkpoint files.
          std::vector<std::filesystem::directory_entry> entries;
          for (auto it = std::filesystem::directory_iterator(ckdir, ec2); !ec2 && it != std::filesystem::directory_iterator(); ++it) {
            const auto& de = *it;
            if (!de.is_regular_file(ec2)) continue;
            const std::string fn = de.path().filename().string();
            if (fn.rfind("structured_", 0) != 0) continue;
            if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;
            entries.push_back(de);
            if (entries.size() > (size_t)keep_checkpoints + 50) break; // bound directory walks
          }
          if (entries.size() > (size_t)keep_checkpoints) {
            std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
              std::error_code eca, ecb;
              const auto ta = a.last_write_time(eca);
              const auto tb = b.last_write_time(ecb);
              if (eca || ecb) return a.path().string() < b.path().string();
              return ta > tb; // newest first
            });
            for (size_t i = (size_t)keep_checkpoints; i < entries.size(); i++) {
              std::filesystem::remove(entries[i].path(), ec2);
              if (ec2) break;
            }
          }
        }
      }
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_put";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["bytes_written"] = (Json::Int64)out_text.size();
  if (structured) data["structured"] = true;
  if (structured) {
    data["entries_valid"] = structured_entries_valid;
    data["entries_changed"] = structured_entries_changed;
    data["entries_superseded"] = structured_entries_superseded;
    data["entries_evidence_added"] = structured_entries_evidence_added;
    data["checkpoint_ok"] = checkpoint_ok;
    if (!checkpoint_path_rel.empty()) data["checkpoint_path"] = checkpoint_path_rel;
    if (checkpoint_ok) {
      if (!checkpoint_ts_utc.empty()) data["checkpoint_ts_utc"] = checkpoint_ts_utc;
      if (!checkpoint_sha256.empty()) data["checkpoint_sha256"] = checkpoint_sha256;
      if (checkpoint_bytes > 0) data["checkpoint_bytes"] = (Json::Int64)checkpoint_bytes;
    }
  }
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  data["output"] = "memory_put: wrote " + rel_path;
  return write_envelope(out_result, true, "", data);
}

static std::vector<std::string> list_candidate_memory_files(const HostToolCtx* ctx, int max_days) {
  std::vector<std::string> out;
  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) return out;

  const std::filesystem::path core = mem_root / "MEMORY.md";
  const std::filesystem::path structured = mem_root / "STRUCTURED.md";
  std::error_code ec;
  if (std::filesystem::exists(structured, ec) && std::filesystem::is_regular_file(structured, ec)) {
    out.push_back(to_generic_string(structured));
  }
  if (std::filesystem::exists(core, ec) && std::filesystem::is_regular_file(core, ec)) {
    out.push_back(to_generic_string(core));
  }

  // Session layer (optional; only when we have a stable session id).
  const std::string sid = trim_ascii(ctx ? ctx->session_id : "");
  if (!sid.empty()) {
    const std::filesystem::path sessionp = mem_root / "sessions" / (sid + ".md");
    ec.clear();
    if (std::filesystem::exists(sessionp, ec) && std::filesystem::is_regular_file(sessionp, ec)) {
      out.push_back(to_generic_string(sessionp));
    }
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

  // Prefer ranked search via an on-disk index (SQLite FTS5) when available.
  const bool use_index = args.isMember("use_index") && args["use_index"].isBool() ? args["use_index"].asBool() : true;
  std::string mode = "substr";
  if (use_index && !case_sensitive) {
    std::vector<MemorySearchHit> hits;
    std::string ierr;
    if (memory_index_search_ranked(mem_root, files, query, max_results, max_snippet_chars, &hits, &ierr)) {
      mode = "fts5";
      for (const auto& h : hits) {
        Json::Value r(Json::objectValue);
        r["path"] = h.path;
        r["line"] = h.line;
        r["snippet"] = h.snippet;
        r["score"] = h.score;
        results.append(r);
      }
    }
  }

  if (mode == "fts5") {
    Json::Value data(Json::objectValue);
    data["tool"] = "memory_search";
    data["mode"] = mode;
    data["memory_root"] = to_generic_string(mem_root);
    data["query"] = query;
    data["files_scanned"] = (Json::Int64)files.size();
    data["results"] = results;
    data["output"] = "memory_search mode=" + mode + " results=" + std::to_string((unsigned)results.size());
    return write_envelope(out_result, true, "", data);
  }

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
  data["mode"] = mode;
  data["memory_root"] = to_generic_string(mem_root);
  data["query"] = query;
  data["files_scanned"] = (Json::Int64)files.size();
  data["results"] = results;
  data["output"] = "memory_search mode=" + mode + " results=" + std::to_string((unsigned)results.size());
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_structured_query(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }

  const std::string rel_path_raw = args.isMember("path") && args["path"].isString() ? trim_ascii(args["path"].asString()) : "STRUCTURED.md";
  const std::string rel_path = rel_path_raw.empty() ? "STRUCTURED.md" : rel_path_raw;
  if (!is_safe_relpath_md(rel_path)) {
    return write_envelope(out_result, false, "invalid field 'path' (must be a safe relative .md path)", Json::Value(Json::objectValue));
  }

  const std::string key = args.isMember("key") && args["key"].isString() ? trim_ascii(args["key"].asString()) : "";
  const std::string key_prefix = args.isMember("key_prefix") && args["key_prefix"].isString() ? trim_ascii(args["key_prefix"].asString()) : "";
  const std::string source_contains =
    args.isMember("source_contains") && args["source_contains"].isString() ? trim_ascii(args["source_contains"].asString()) : "";

  const bool key_case_insensitive =
    args.isMember("key_case_insensitive") && args["key_case_insensitive"].isBool() ? args["key_case_insensitive"].asBool() : false;
  const bool source_case_insensitive =
    args.isMember("source_case_insensitive") && args["source_case_insensitive"].isBool() ? args["source_case_insensitive"].asBool() : false;

  std::vector<std::string> kinds;
  if (args.isMember("kinds") && args["kinds"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["kinds"].size(); i++) {
      if (!args["kinds"][i].isString()) continue;
      const std::string k = to_lower_ascii(trim_ascii(args["kinds"][i].asString()));
      if (k.empty()) continue;
      if (k == "pref") kinds.push_back("preference");
      else kinds.push_back(k);
    }
  }

  const std::string status_filter_raw =
    args.isMember("status") && args["status"].isString() ? to_lower_ascii(trim_ascii(args["status"].asString())) : "active";
  const std::string status_filter = status_filter_raw.empty() ? "active" : status_filter_raw;

  const bool include_sources =
    args.isMember("include_sources") && args["include_sources"].isBool() ? args["include_sources"].asBool() : true;
  const bool include_versions =
    args.isMember("include_versions") && args["include_versions"].isBool() ? args["include_versions"].asBool() : false;

  int limit = args.isMember("limit") && args["limit"].isInt() ? args["limit"].asInt() : 50;
  limit = std::max(1, std::min(200, limit));

  if (key.empty() && key_prefix.empty() && kinds.empty() && source_contains.empty()) {
    return write_envelope(
      out_result,
      false,
      "memory_structured_query requires at least one filter: key, key_prefix, non-empty kinds[], or source_contains",
      Json::Value(Json::objectValue)
    );
  }
  if (source_contains.size() > 300) {
    return write_envelope(out_result, false, "source_contains too long (max 300 chars)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_structured_query requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }
  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }

  std::string content;
  bool missing = false;
  {
    std::ifstream in(abs, std::ios::binary);
    if (!in.is_open()) {
      missing = true;
      content = "";
    } else {
      std::stringstream ss;
      ss << in.rdbuf();
      content = ss.str();
      if (content.size() > 2 * 1024 * 1024) {
        content.resize(2 * 1024 * 1024);
      }
    }
  }

  Json::Value doc;
  std::string derr;
  if (!parse_structured_memory_doc(content, &doc, &derr)) {
    return write_envelope(out_result, false, std::string("failed to parse structured memory doc: ") + derr, Json::Value(Json::objectValue));
  }

  const Json::Value items = doc.isMember("items") ? doc["items"] : Json::Value(Json::nullValue);
  if (!items.isObject()) {
    return write_envelope(out_result, false, "structured memory doc missing items", Json::Value(Json::objectValue));
  }

  auto key_norm = [&](const std::string& s) -> std::string { return key_case_insensitive ? to_lower_ascii(s) : s; };
  const std::string key_n = key_norm(key);
  const std::string pref_n = key_norm(key_prefix);
  auto source_norm = [&](const std::string& s) -> std::string { return source_case_insensitive ? to_lower_ascii(s) : s; };
  const std::string src_n = source_norm(source_contains);

  auto kind_match = [&](const std::string& kraw) -> bool {
    if (kinds.empty()) return true;
    const std::string k = to_lower_ascii(trim_ascii(kraw));
    for (const auto& want : kinds) {
      if (want == k) return true;
    }
    return false;
  };
  auto status_match = [&](const std::string& sraw) -> bool {
    if (status_filter == "any" || status_filter == "*") return true;
    const std::string s = to_lower_ascii(trim_ascii(sraw.empty() ? "active" : sraw));
    return s == status_filter;
  };
  auto prefix_match = [&](const std::string& k) -> bool {
    if (pref_n.empty()) return true;
    const std::string kn = key_norm(k);
    if (kn.size() < pref_n.size()) return false;
    return kn.compare(0, pref_n.size(), pref_n) == 0;
  };
  auto sources_match = [&](const Json::Value& rec) -> bool {
    if (src_n.empty()) return true;
    const Json::Value sources = rec.isMember("sources") ? rec["sources"] : Json::Value(Json::nullValue);
    if (!sources.isArray()) return false;
    for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
      if (!sources[i].isString()) continue;
      const std::string s = source_norm(sources[i].asString());
      if (s.find(src_n) != std::string::npos) return true;
    }
    return false;
  };

  Json::Value results(Json::arrayValue);
  int matched = 0;
  for (const auto& k : items.getMemberNames()) {
    if ((int)results.size() >= limit) break;
    if (!key_n.empty()) {
      if (key_norm(k) != key_n) continue;
    } else if (!prefix_match(k)) {
      continue;
    }
    const Json::Value rec = items[k];
    if (!rec.isObject()) continue;
    const std::string kind = rec.isMember("kind") && rec["kind"].isString() ? rec["kind"].asString() : "fact";
    const std::string status = rec.isMember("status") && rec["status"].isString() ? rec["status"].asString() : "active";
    if (!kind_match(kind)) continue;
    if (!status_match(status)) continue;
    if (!sources_match(rec)) continue;

    Json::Value out_rec = rec;
    if (!include_versions && out_rec.isMember("versions")) out_rec.removeMember("versions");
    if (!include_sources && out_rec.isMember("sources")) out_rec.removeMember("sources");
    if (out_rec.isMember("source")) out_rec.removeMember("source"); // v1 legacy field (keep output v2-clean)

    Json::Value row(Json::objectValue);
    row["key"] = k;
    row["record"] = out_rec;
    results.append(row);
    matched++;

    if (!key_n.empty() && key_norm(k) == key_n) break;
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_structured_query";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["missing"] = missing;
  Json::Value q(Json::objectValue);
  if (!key.empty()) q["key"] = key;
  if (!key_prefix.empty()) q["key_prefix"] = key_prefix;
  q["key_case_insensitive"] = key_case_insensitive;
  if (!source_contains.empty()) q["source_contains"] = source_contains;
  q["source_case_insensitive"] = source_case_insensitive;
  q["status"] = status_filter;
  q["include_sources"] = include_sources;
  q["include_versions"] = include_versions;
  q["limit"] = limit;
  if (!kinds.empty()) {
    Json::Value ka(Json::arrayValue);
    for (const auto& k : kinds) ka.append(k);
    q["kinds"] = ka;
  }
  data["query"] = q;
  data["matched"] = matched;
  data["results"] = results;
  data["output"] = "memory_structured_query: matched=" + std::to_string(matched);
  return write_envelope(out_result, true, "", data);
}

#endif  // AGENT_HAVE_JSONCPP

} // namespace host_tools_internal
