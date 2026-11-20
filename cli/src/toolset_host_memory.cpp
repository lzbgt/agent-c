#include "toolset_host_internal.h"

#include "memory_index.h"
#include "memory_store.h"
#include "agent_sha256.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#include "memory_structured.h"
#include "memory_view.h"
#include "memory_promotion.h"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace host_tools_internal {

#if !defined(AGENT_HAVE_JSONCPP)
agent_status_t tool_memory_write(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_write requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_observe(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_observe requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_get(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_get requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_search(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_search requires jsoncpp\",\"data\":{}}");
}
agent_status_t tool_memory_timeline(HostToolCtx*, const char*, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"memory_timeline requires jsoncpp\",\"data\":{}}");
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

static int64_t daily_date_key_from_path(const std::string& rel_path) {
  const std::string base = std::filesystem::path(rel_path).filename().string();
  if (base.size() != 13) return 0;
  if (base[4] != '-' || base[7] != '-' || base.rfind(".md") != 10) return 0;
  auto parse_num = [&](size_t pos, size_t len) -> int {
    int v = 0;
    for (size_t i = 0; i < len; i++) {
      const char c = base[pos + i];
      if (c < '0' || c > '9') return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const int y = parse_num(0, 4);
  const int m = parse_num(5, 2);
  const int d = parse_num(8, 2);
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return (int64_t)y * 10000 + (int64_t)m * 100 + (int64_t)d;
}

static int64_t daily_timeline_key_from_result(const Json::Value& r) {
  if (!r.isObject()) return 0;
  if (!r.isMember("tier") || !r["tier"].isString()) return 0;
  if (r["tier"].asString() != "daily") return 0;
  if (!r.isMember("path") || !r["path"].isString()) return 0;
  const std::string path = r["path"].asString();
  const int64_t ymd = daily_date_key_from_path(path);
  if (ymd <= 0) return 0;
  const int line = r.isMember("line") && r["line"].isInt() ? r["line"].asInt() : 0;
  const int64_t ln = line > 0 ? line : 0;
  return ymd * 1000000 + ln;
}

static void apply_timeline_order(Json::Value* results, const std::string& order) {
  if (!results || !results->isArray()) return;
  if (order == "ranked") return;
  const bool newest = (order == "newest");
  std::vector<Json::ArrayIndex> daily_indices;
  struct DailyItem {
    int64_t key = 0;
    Json::Value row;
  };
  std::vector<DailyItem> daily_rows;
  for (Json::ArrayIndex i = 0; i < results->size(); i++) {
    const Json::Value& r = (*results)[i];
    const int64_t key = daily_timeline_key_from_result(r);
    if (key <= 0) continue;
    daily_indices.push_back(i);
    DailyItem item;
    item.key = key;
    item.row = r;
    daily_rows.push_back(std::move(item));
  }
  if (daily_rows.size() <= 1) return;
  std::sort(daily_rows.begin(), daily_rows.end(), [&](const DailyItem& a, const DailyItem& b) {
    return newest ? (a.key > b.key) : (a.key < b.key);
  });
  for (size_t i = 0; i < daily_indices.size(); i++) {
    (*results)[daily_indices[i]] = daily_rows[i].row;
  }
}

std::string memory_strip_private_blocks(const std::string& input, int* out_stripped_bytes) {
  if (out_stripped_bytes) *out_stripped_bytes = 0;
  if (input.empty()) return input;
  const std::string lower = to_lower_ascii(input);
  const std::string open_tag = "<private>";
  const std::string close_tag = "</private>";
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    const size_t open = lower.find(open_tag, i);
    if (open == std::string::npos) {
      out.append(input.substr(i));
      break;
    }
    out.append(input.substr(i, open - i));
    const size_t close = lower.find(close_tag, open + open_tag.size());
    if (close == std::string::npos) {
      if (out_stripped_bytes) *out_stripped_bytes += (int)(input.size() - open);
      break;
    }
    if (out_stripped_bytes) *out_stripped_bytes += (int)((close + close_tag.size()) - open);
    i = close + close_tag.size();
  }
  return out;
}

static std::string sanitize_inline_field(std::string s) {
  for (char& c : s) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  }
  return trim_ascii(s);
}

static std::string format_bullet_multiline(const std::string& text) {
  std::string out;
  std::istringstream iss(text);
  std::string line;
  bool first = true;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      out += line;
      first = false;
    } else {
      out += "\n  ";
      out += line;
    }
  }
  if (out.empty()) out = text;
  return out;
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

static bool is_iso_utc_string(const std::string& s) {
  // Expected: YYYY-MM-DDTHH:MM:SSZ
  if (s.empty()) return true;
  if (s.size() != 20) return false;
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
  for (size_t i = 0; i < s.size(); i++) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
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
  if (!ctx->memory_root_override.empty()) {
    return ctx->memory_root_override;
  }
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

static void json_array_append_unique_strings_ci(Json::Value* arr, const Json::Value& values, int max_items) {
  if (!arr || !values.isArray()) return;
  for (Json::ArrayIndex i = 0; i < values.size(); i++) {
    if (!values[i].isString()) continue;
    json_array_append_unique_string_ci(arr, values[i].asString(), max_items);
  }
}

static bool collect_string_field_or_array(
  const Json::Value& obj,
  const char* field,
  Json::Value* out_values,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_values || !field || !obj.isObject() || !obj.isMember(field) || obj[field].isNull()) return true;
  const Json::Value& v = obj[field];
  if (v.isString()) {
    const std::string s = trim_ascii(v.asString());
    if (!s.empty()) out_values->append(s);
    return true;
  }
  if (v.isArray()) {
    for (Json::ArrayIndex i = 0; i < v.size(); i++) {
      if (!v[i].isString()) {
        if (out_err) *out_err = std::string(field) + " must contain only strings";
        return false;
      }
      const std::string s = trim_ascii(v[i].asString());
      if (!s.empty()) out_values->append(s);
    }
    return true;
  }
  if (out_err) *out_err = std::string(field) + " must be a string or array of strings";
  return false;
}

static bool read_optional_iso_utc_field(
  const Json::Value& obj,
  const char* field,
  std::string* out_value,
  std::string* out_err
) {
  if (out_value) out_value->clear();
  if (out_err) out_err->clear();
  if (!field || !obj.isObject() || !obj.isMember(field) || obj[field].isNull()) return true;
  if (!obj[field].isString()) {
    if (out_err) *out_err = std::string(field) + " must be an ISO UTC string";
    return false;
  }
  const std::string v = trim_ascii(obj[field].asString());
  if (!is_iso_utc_string(v)) {
    if (out_err) *out_err = std::string(field) + " must be ISO UTC like 2026-02-05T00:00:00Z";
    return false;
  }
  if (out_value) *out_value = v;
  return true;
}

static std::string structured_record_supersedes_ref(const std::string& key, const Json::Value& rec) {
  const std::string updated =
    rec.isObject() && rec.isMember("updated_utc") && rec["updated_utc"].isString()
      ? trim_ascii(rec["updated_utc"].asString())
      : "";
  return key + "@" + (updated.empty() ? "previous" : updated);
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

// Decode the stored object without migration: evidence pointers must address
// fields that actually exist in these bytes. Delimiters inside JSON strings are data.
bool memory_structured_decode(const std::string &text, Json::Value *doc, std::string *error,
                              size_t *block_end) {
  if (error)
    error->clear();
  if (block_end)
    *block_end = 0;
  if (!doc)
    return false;
  *doc = Json::Value(Json::objectValue);
  const auto fail = [&](const std::string &why) {
    if (error)
      *error = why;
    return false;
  };
  size_t begin = std::string::npos;
  for (size_t pos = 0; pos < text.size();) {
    const auto end = text.find('\n', pos);
    if (trim_ascii(text.substr(pos, end == std::string::npos ? end : end - pos)) ==
        kAgentMemoryV1Begin) {
      begin = end == std::string::npos ? text.size() : end + 1;
      break;
    }
    if (end == std::string::npos)
      break;
    pos = end + 1;
  }
  if (begin == std::string::npos) {
    if (text.find(kAgentMemoryV1Begin) != std::string::npos ||
        text.find(kAgentMemoryV1End) != std::string::npos)
      return fail("incomplete structured memory machine block");
    (*doc)["schema"] = "agent_memory_v2";
    (*doc)["items"] = Json::Value(Json::objectValue);
    return true;
  }
  auto first = text.find_first_not_of(" \t\r\n", begin);
  if (first == std::string::npos || text[first] != '{')
    return fail("expected structured memory JSON object");
  bool quoted = false, escaped = false;
  size_t end = first;
  int depth = 0;
  for (; end < text.size(); ++end) {
    const char c = text[end];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 128)
        return fail("structured memory JSON depth exceeds 128");
    } else if (c == '}' || c == ']') {
      if (--depth == 0) {
        ++end;
        break;
      }
    }
  }
  const auto marker = text.find_first_not_of(" \t\r\n", end);
  if (depth != 0 || quoted || marker == std::string::npos ||
      text.compare(marker, std::string(kAgentMemoryV1End).size(), kAgentMemoryV1End) != 0)
    return fail("incomplete structured memory machine block");
  const size_t stop = marker + std::string(kAgentMemoryV1End).size();
  if (stop < text.size() && text[stop] != '\r' && text[stop] != '\n')
    return fail("invalid structured memory end marker");
  Json::CharReaderBuilder reader;
  reader["rejectDupKeys"] = true;
  reader["failIfExtra"] = true;
  reader["allowComments"] = false;
  reader["stackLimit"] = 128;
  std::istringstream input(text.substr(first, end - first));
  if (!Json::parseFromStream(reader, input, doc, error) || !doc->isObject())
    return fail("invalid structured memory JSON object");
  if (block_end)
    *block_end = stop;
  return true;
}
bool memory_structured_parse_doc(const std::string &text, Json::Value *doc, std::string *error) {
  if (!memory_structured_decode(text, doc, error))
    return false;
  if ((*doc)["schema"] != "agent_memory_v2" || !(*doc)["items"].isObject()) {
    if (error)
      *error = "current structured memory requires agent_memory_v2 and an items object";
    return false;
  }
  return true;
}
static std::string extract_user_notes_md(const std::string &text) {
  Json::Value original;
  std::string error;
  size_t end = 0;
  if (!memory_structured_decode(text, &original, &error, &end))
    return "";
  const auto notes =
      extract_block_body(text.substr(end), kAgentMemoryV1NotesBegin, kAgentMemoryV1NotesEnd);
  return trim_ascii(notes).empty() ? "" : notes;
}

static agent_status_t memory_store_error(agent_string_t* out, const std::string& tool,
    MemoryStoreStatus status, const std::string& error, const MemoryDocument& document = {},
    const std::string& expected = "") {
  Json::Value data;data["tool"] = tool;
  data["code"] = status == MemoryStoreStatus::Conflict ? "memory_revision_conflict" :
    status == MemoryStoreStatus::Busy ? "memory_busy" : status == MemoryStoreStatus::Cancelled ? "cancelled" :
    status == MemoryStoreStatus::Uncertain ? "memory_publication_uncertain" : "memory_unavailable";
  data["commit_state"] = status == MemoryStoreStatus::Uncertain ? "published_unconfirmed" : "not_written";
  if (!document.source_id.empty()) { data["source_id"] = document.source_id;data["current_revision"] = document.revision; }
  if (!expected.empty()) data["expected_revision"] = expected;
  const auto written = write_envelope(out, false, error, data);
  // An unconfirmed local write is not a terminal executor receipt. The existing
  // model/workflow journal must retain its uncertainty instead of replaying it.
  return status == MemoryStoreStatus::Uncertain && written == AGENT_OK ? AGENT_ERR_INTERNAL : written;
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
  if (!ctx->memory_write_allowed) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_write";
    d["memory_scope"] = "read_only";
    return write_envelope(out_result, false, "memory scope is read_only", d);
  }

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }
  const std::string raw_text = args.isMember("text") && args["text"].isString() ? args["text"].asString() : "";
  int private_bytes_stripped = 0;
  const std::string text = memory_strip_private_blocks(raw_text, &private_bytes_stripped);
  if (trim_ascii(raw_text).empty()) {
    return write_envelope(out_result, false, "missing string field 'text'", Json::Value(Json::objectValue));
  }
  if (trim_ascii(text).empty()) {
    Json::Value data(Json::objectValue);
    data["tool"] = "memory_write";
    data["skipped_private"] = true;
    data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
    data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
    data["output"] = "memory_write: skipped private-only entry";
    return write_envelope(out_result, true, "", data);
  }

  const std::string layer = args.isMember("layer") && args["layer"].isString() ? args["layer"].asString() : "daily";
  const std::string rel_path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";
  const std::filesystem::path out_path = memory_file_for_layer(ctx, layer, rel_path);
  if (out_path.empty()) {
    return write_envelope(out_result, false, "memory_write requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
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

  MemoryDocumentTransaction transaction;std::string store_error;
  auto store_status = transaction.begin(mem_root, out_path.lexically_relative(mem_root).generic_string(), std::nullopt,
    [ctx] { return is_cancelled(ctx); }, &store_error);
  if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_write", store_status, store_error, transaction.document());
  if (transaction.document().text.find(kAgentMemoryV1Begin) != std::string::npos)
    return write_envelope(out_result, false, "structured memory requires revision-checked memory_put entries", Json::Value(Json::objectValue));
  const auto previous_revision = transaction.document().revision;
  store_status = transaction.commit(transaction.document().text + entry, &store_error);
  if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_write", store_status, store_error, transaction.document(), previous_revision);

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_write";
  data["layer"] = layer;
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = to_generic_string(out_path.lexically_relative(mem_root));
  data["abs_path"] = to_generic_string(out_path);
  data["bytes_appended"] = (Json::Int64)entry.size();
  data["previous_revision"] = previous_revision;data["revision"] = transaction.document().revision;
  data["source_id"] = transaction.document().source_id;
  if (private_bytes_stripped > 0) {
    data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
  }
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  data["output"] = "memory_write: appended to " + data["path"].asString();
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_observe(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  if (ctx->policy != HostToolsetPolicyMode::Full) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_observe";
    d["policy"] = "readonly";
    return write_envelope(out_result, false, "tool disabled by policy", d);
  }
  if (!ctx->memory_write_allowed) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_observe";
    d["memory_scope"] = "read_only";
    return write_envelope(out_result, false, "memory scope is read_only", d);
  }

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }

  const std::string raw_text = args.isMember("text") && args["text"].isString() ? args["text"].asString() : "";
  int private_bytes_stripped = 0;
  const std::string text = memory_strip_private_blocks(raw_text, &private_bytes_stripped);
  if (trim_ascii(raw_text).empty()) {
    return write_envelope(out_result, false, "missing string field 'text'", Json::Value(Json::objectValue));
  }
  if (trim_ascii(text).empty()) {
    Json::Value data(Json::objectValue);
    data["tool"] = "memory_observe";
    data["skipped_private"] = true;
    data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
    data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
    data["output"] = "memory_observe: skipped private-only entry";
    return write_envelope(out_result, true, "", data);
  }

  const std::filesystem::path out_path = memory_file_for_layer(ctx, "daily", "");
  if (out_path.empty()) {
    return write_envelope(out_result, false, "memory_observe requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  const std::string source =
    args.isMember("source") && args["source"].isString() ? sanitize_inline_field(args["source"].asString()) : "";
  const std::string trace_id =
    args.isMember("trace_id") && args["trace_id"].isString() ? sanitize_inline_field(args["trace_id"].asString()) : "";
  const std::string citation =
    args.isMember("citation") && args["citation"].isString() ? sanitize_inline_field(args["citation"].asString()) : "";
  const int importance_raw = args.isMember("importance") && args["importance"].isInt() ? args["importance"].asInt() : -1;
  const int importance = std::min(std::max(importance_raw, -1), 5);

  std::vector<std::string> tags;
  if (args.isMember("tags") && args["tags"].isArray()) {
    const Json::Value& arr = args["tags"];
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      if (!arr[i].isString()) continue;
      const std::string tag = sanitize_inline_field(arr[i].asString());
      if (tag.empty()) continue;
      tags.push_back(tag);
    }
  }

  std::ostringstream entry;
  entry << "\n\n- @obs " << format_bullet_multiline(trim_ascii(text)) << "\n";
  entry << "  - ts_utc: " << iso_utc_now() << "\n";
  if (!source.empty()) entry << "  - source: " << source << "\n";
  if (!trace_id.empty()) entry << "  - trace_id: " << trace_id << "\n";
  if (!citation.empty()) entry << "  - citation: " << citation << "\n";
  if (!tags.empty()) {
    entry << "  - tags: ";
    for (size_t i = 0; i < tags.size(); i++) {
      if (i > 0) entry << ", ";
      entry << tags[i];
    }
    entry << "\n";
  }
  if (importance >= 0) {
    entry << "  - importance: " << importance << "\n";
  }

  const std::string entry_str = entry.str();
  MemoryDocumentTransaction transaction;std::string store_error;
  auto store_status = transaction.begin(mem_root, out_path.lexically_relative(mem_root).generic_string(), std::nullopt,
    [ctx] { return is_cancelled(ctx); }, &store_error);
  if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_observe", store_status, store_error, transaction.document());
  if (transaction.document().text.find(kAgentMemoryV1Begin) != std::string::npos)
    return write_envelope(out_result, false, "structured memory requires revision-checked memory_put entries", Json::Value(Json::objectValue));
  const auto previous_revision = transaction.document().revision;
  store_status = transaction.commit(transaction.document().text + entry_str, &store_error);
  if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_observe", store_status, store_error, transaction.document(), previous_revision);

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_observe";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = to_generic_string(out_path.lexically_relative(mem_root));
  data["abs_path"] = to_generic_string(out_path);
  data["bytes_appended"] = (Json::Int64)entry_str.size();
  data["previous_revision"] = previous_revision;data["revision"] = transaction.document().revision;
  data["source_id"] = transaction.document().source_id;
  if (private_bytes_stripped > 0) {
    data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
  }
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  data["output"] = "memory_observe: appended to " + data["path"].asString();
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

  if (args.isMember("evidence")) {
    std::string text, error;Json::Value value;
    if (!memory_evidence_read(memory_root_from_ctx(ctx), args["evidence"], &text, &value, &error))
      return write_envelope(out_result, false, error, Json::Value(Json::objectValue));
    Json::Value data;data["tool"] = "memory_get";data["view"] = "evidence";data["text"] = text;data["value"] = value;
    data["evidence"] = args["evidence"];data["revision"] = args["evidence"]["revision"];data["source_id"] = args["evidence"]["source_id"];
    return write_envelope(out_result, true, "", data);
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

  MemoryDocument document;std::string store_error;
  if (args.isMember("revision") && (!args["revision"].isString() || !memory_revision_valid(args["revision"].asString()) || args["revision"].asString() == "absent"))
    return write_envelope(out_result, false, "revision must identify retained content", Json::Value(Json::objectValue));
  const auto status = args.isMember("revision")
    ? memory_document_read_revision(mem_root, rel_path, args["revision"].asString(), &document, &store_error)
    : memory_document_read(mem_root, rel_path, &document, &store_error);
  if (status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_get", status, store_error);
  if (!document.exists) {
    Json::Value data;data["tool"] = "memory_get";data["path"] = rel_path;data["exists"] = false;
    data["revision"] = document.revision;data["source_id"] = document.source_id;
    return write_envelope(out_result, false, "memory document does not exist", data);
  }
  const auto& content = document.text;

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
  data["view"] = "evidence";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["from_line"] = from;
  data["max_lines"] = max_lines;
  data["text"] = out_text;
  data["exists"] = true;data["revision"] = document.revision;data["source_id"] = document.source_id;
  data["output"] = "memory_get: " + rel_path;
  return write_envelope(out_result, true, "", data);
}

static agent_status_t memory_put_impl(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result, const MemoryPromotionPlan* promotion) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  if (ctx->policy != HostToolsetPolicyMode::Full) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_put";
    d["policy"] = "readonly";
    return write_envelope(out_result, false, "tool disabled by policy", d);
  }
  if (!ctx->memory_write_allowed) {
    Json::Value d(Json::objectValue);
    d["tool_name"] = "memory_put";
    d["memory_scope"] = "read_only";
    return write_envelope(out_result, false, "memory scope is read_only", d);
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
  int private_bytes_stripped = 0;
  int private_entries_skipped = 0;
  const std::string raw_text = (!structured && args.isMember("text") && args["text"].isString()) ? args["text"].asString() : "";
  const std::string text = structured ? "" : memory_strip_private_blocks(raw_text, &private_bytes_stripped);
  // Legacy mode: allow empty text (truncate file to empty), but require the field.
  if (!structured && (!args.isMember("text") || !args["text"].isString())) {
    return write_envelope(out_result, false, "missing string field 'text'", Json::Value(Json::objectValue));
  }
  if (!structured && trim_ascii(raw_text).size() > 0 && trim_ascii(text).empty()) {
    Json::Value data(Json::objectValue);
    data["tool"] = "memory_put";
    data["memory_root"] = to_generic_string(memory_root_from_ctx(ctx));
    data["path"] = rel_path;
    data["structured"] = false;
    data["skipped_private"] = true;
    data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
    data["bytes_written"] = (Json::Int64)0;
    data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
    data["output"] = "memory_put: skipped private-only content";
    return write_envelope(out_result, true, "", data);
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_put requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }
  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!args["expected_revision"].isString() || !memory_revision_valid(args["expected_revision"].asString()))
    return write_envelope(out_result, false, "expected_revision is required: use memory_get data.revision, or absent for a new source", Json::Value(Json::objectValue));
  const auto expected_revision = args["expected_revision"].asString();
  MemoryDocumentTransaction transaction;std::string store_error;
  auto store_status = transaction.begin(mem_root, rel_path, expected_revision, [ctx] { return is_cancelled(ctx); }, &store_error);
  if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_put", store_status, store_error, transaction.document(), expected_revision);

  if (!structured && transaction.document().text.find("<!-- AGENT_MEMORY_V1_BEGIN -->") != std::string::npos)
    return write_envelope(out_result, false, "structured sources require entries; full-text replacement could erase durable receipts", Json::Value(Json::objectValue));
  MemoryPromotionUpdate promotion_update;
  bool metadata_changed = false;
  std::string out_text;
  Json::Value structured_doc_for_checkpoint(Json::objectValue);
  bool structured_doc_present = false;
  int structured_entries_valid = 0;
  int structured_entries_changed = 0;
  int structured_entries_superseded = 0;
  int structured_entries_evidence_added = 0;
  if (structured) {
    // Structured upsert mode: deterministic key conflict resolution for durable facts/preferences/tasks.
    const auto& existing = transaction.document().text;

    Json::Value doc;
    std::string derr;
    if (!memory_structured_parse_doc(existing, &doc, &derr)) {
      return write_envelope(out_result, false, std::string("failed to parse structured memory doc: ") + derr, Json::Value(Json::objectValue));
    }

    if (!memory_promotion_ledger_valid(doc, &derr))
      return write_envelope(out_result, false, derr, Json::Value(Json::objectValue));
    if (promotion && !memory_promotion_prepare(mem_root, doc, *promotion, &promotion_update, &derr))
      return write_envelope(out_result, false, derr, Json::Value(Json::objectValue));
    // Preserve observations when converting an ordinary Markdown source.
    const std::string user_notes = existing.find("<!-- AGENT_MEMORY_V1_BEGIN -->") == std::string::npos
      ? existing : extract_user_notes_md(existing);
    const auto& entries = promotion ? promotion_update.entries : args["entries"];
    if (!entries.isArray() || (entries.empty() && !promotion)) {
      return write_envelope(out_result, false, "entries must be a non-empty array", Json::Value(Json::objectValue));
    }

    Json::Value& items = doc["items"];
    if (!items.isObject()) items = Json::Value(Json::objectValue);
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      const auto& e = entries[i];
      if (!e.isObject()) continue;
      const std::string key = e.isMember("key") && e["key"].isString() ? trim_ascii(e["key"].asString()) : "";
      const std::string kind = e.isMember("kind") && e["kind"].isString() ? trim_ascii(e["kind"].asString()) : "fact";
      const std::string raw_value = e.isMember("value") && e["value"].isString() ? e["value"].asString() : "";
      int stripped = 0;
      const std::string value = memory_strip_private_blocks(raw_value, &stripped);
      if (stripped > 0) private_bytes_stripped += stripped;
      if (!trim_ascii(raw_value).empty() && trim_ascii(value).empty()) {
        private_entries_skipped++;
        continue;
      }
      const std::string status = e.isMember("status") && e["status"].isString() ? trim_ascii(e["status"].asString()) : "active";
      const std::string source = e.isMember("source") && e["source"].isString() ? trim_ascii(e["source"].asString()) : "";
      Json::Value entry_sources(Json::arrayValue);
      if (!source.empty()) json_array_append_unique_string_ci(&entry_sources, source, 20);
      std::string field_err;
      if (!collect_string_field_or_array(e, "sources", &entry_sources, &field_err)) {
        return write_envelope(out_result, false, field_err, Json::Value(Json::objectValue));
      }
      Json::Value entry_supersedes(Json::arrayValue);
      if (!collect_string_field_or_array(e, "supersedes", &entry_supersedes, &field_err)) {
        return write_envelope(out_result, false, field_err, Json::Value(Json::objectValue));
      }
      std::string observed_utc;
      if (!read_optional_iso_utc_field(e, "observed_utc", &observed_utc, &field_err)) {
        return write_envelope(out_result, false, field_err, Json::Value(Json::objectValue));
      }
      std::string valid_from;
      if (!read_optional_iso_utc_field(e, "valid_from", &valid_from, &field_err)) {
        return write_envelope(out_result, false, field_err, Json::Value(Json::objectValue));
      }
      if (valid_from.empty()) {
        if (!read_optional_iso_utc_field(e, "valid_from_utc", &valid_from, &field_err)) {
          return write_envelope(out_result, false, field_err, Json::Value(Json::objectValue));
        }
      }
      if (key.empty() || value.empty()) continue;
      structured_entries_valid++;

      const std::string norm_kind = trim_ascii(kind).empty() ? "fact" : kind;
      const std::string norm_status = trim_ascii(status).empty() ? "active" : status;
      const std::string now_utc = iso_utc_now();
      const std::string incoming_observed_utc = observed_utc.empty() ? now_utc : observed_utc;

      if (!items.isMember(key) || !items[key].isObject()) {
        Json::Value rec(Json::objectValue);
        rec["kind"] = norm_kind;
        rec["value"] = value;
        rec["status"] = norm_status;
        rec["updated_utc"] = now_utc;
        rec["observed_utc"] = incoming_observed_utc;
        if (!valid_from.empty()) rec["valid_from"] = valid_from;
        rec["versions"] = Json::Value(Json::arrayValue);
        rec["sources"] = Json::Value(Json::arrayValue);
        json_array_append_unique_strings_ci(&rec["sources"], entry_sources, 20);
        if (!entry_supersedes.empty()) {
          rec["supersedes"] = Json::Value(Json::arrayValue);
          json_array_append_unique_strings_ci(&rec["supersedes"], entry_supersedes, 20);
        }
        items[key] = rec;
        structured_entries_changed++;
        continue;
      }

      Json::Value& cur = items[key];
      if (!cur.isMember("versions") || !cur["versions"].isArray()) cur["versions"] = Json::Value(Json::arrayValue);
      if (!cur.isMember("sources") || !cur["sources"].isArray()) cur["sources"] = Json::Value(Json::arrayValue);
      if (cur.isMember("supersedes") && !cur["supersedes"].isArray()) {
        Json::Value arr(Json::arrayValue);
        if (cur["supersedes"].isString() && !trim_ascii(cur["supersedes"].asString()).empty()) {
          arr.append(trim_ascii(cur["supersedes"].asString()));
        }
        cur["supersedes"] = arr;
      }
      if (!cur.isMember("observed_utc") || !cur["observed_utc"].isString()) {
        cur["observed_utc"] = cur.isMember("updated_utc") && cur["updated_utc"].isString() ? cur["updated_utc"] : now_utc;
      }

      const std::string cur_kind = cur.isMember("kind") && cur["kind"].isString() ? cur["kind"].asString() : "fact";
      const std::string cur_value = cur.isMember("value") && cur["value"].isString() ? cur["value"].asString() : "";
      const std::string cur_status = cur.isMember("status") && cur["status"].isString() ? cur["status"].asString() : "active";
      const std::string cur_valid_from = cur.isMember("valid_from") && cur["valid_from"].isString() ? cur["valid_from"].asString() : "";
      const std::string next_valid_from = valid_from.empty() ? cur_valid_from : valid_from;

      const bool same_core =
        eq_ci_ascii(cur_kind, norm_kind) && cur_value == value && eq_ci_ascii(cur_status, norm_status) && cur_valid_from == next_valid_from;
      bool need_add_source = false;
      for (Json::ArrayIndex si = 0; si < entry_sources.size(); si++) {
        if (!entry_sources[si].isString()) continue;
        if (!json_array_contains_string_ci(cur["sources"], entry_sources[si].asString())) {
          need_add_source = true;
          break;
        }
      }
      bool need_add_supersedes = false;
      for (Json::ArrayIndex si = 0; si < entry_supersedes.size(); si++) {
        if (!entry_supersedes[si].isString()) continue;
        const Json::Value cur_sup = cur.isMember("supersedes") ? cur["supersedes"] : Json::Value(Json::arrayValue);
        if (!json_array_contains_string_ci(cur_sup, entry_supersedes[si].asString())) {
          need_add_supersedes = true;
          break;
        }
      }
      const bool need_update_observed =
        !observed_utc.empty() && (!cur.isMember("observed_utc") || !cur["observed_utc"].isString() || cur["observed_utc"].asString() != observed_utc);

      if (same_core) {
        if (!need_add_source && !need_add_supersedes && !need_update_observed) continue;
        if (need_add_source) {
          json_array_append_unique_strings_ci(&cur["sources"], entry_sources, 20);
          structured_entries_evidence_added++;
        }
        if (need_add_supersedes) {
          if (!cur.isMember("supersedes") || !cur["supersedes"].isArray()) cur["supersedes"] = Json::Value(Json::arrayValue);
          json_array_append_unique_strings_ci(&cur["supersedes"], entry_supersedes, 20);
        }
        cur["observed_utc"] = incoming_observed_utc;
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
      if (cur.isMember("valid_from")) prev["valid_from"] = cur["valid_from"];
      if (cur.isMember("supersedes")) prev["supersedes"] = cur["supersedes"];
      if (cur.isMember("sources") && cur["sources"].isArray()) prev["sources"] = cur["sources"];
      if (cur.isMember("derivation")) prev["derivation"] = cur["derivation"];
      cur.removeMember("derivation");
      prev["superseded_utc"] = now_utc;

      Json::Value& vers = cur["versions"];
      vers.insert(0U, prev); // newest first
      while ((int)vers.size() > 20) vers.removeIndex((Json::ArrayIndex)vers.size() - 1, nullptr);

      cur["kind"] = norm_kind;
      cur["value"] = value;
      cur["status"] = norm_status;
      cur["updated_utc"] = now_utc;
      cur["observed_utc"] = incoming_observed_utc;
      if (!valid_from.empty()) cur["valid_from"] = valid_from;
      else if (cur.isMember("valid_from")) cur.removeMember("valid_from");
      cur["sources"] = Json::Value(Json::arrayValue);
      json_array_append_unique_strings_ci(&cur["sources"], entry_sources, 20);
      cur["supersedes"] = Json::Value(Json::arrayValue);
      json_array_append_unique_string_ci(&cur["supersedes"], structured_record_supersedes_ref(key, prev), 20);
      json_array_append_unique_strings_ci(&cur["supersedes"], entry_supersedes, 20);
      if (cur["supersedes"].empty()) cur.removeMember("supersedes");
      structured_entries_superseded++;
      structured_entries_changed++;
    }
    if (promotion && !memory_promotion_finish(&doc, *promotion, &promotion_update, &metadata_changed, &derr))
      return write_envelope(out_result, false, derr, Json::Value(Json::objectValue));
    if (structured_entries_valid == 0 && !promotion) {
      return write_envelope(out_result, false, "no valid entries applied (each entry requires key + value)", Json::Value(Json::objectValue));
    }
    if (structured_entries_changed == 0 && !metadata_changed) {
      Json::Value data(Json::objectValue);
      data["tool"] = "memory_put";
      data["memory_root"] = to_generic_string(mem_root);
      data["path"] = rel_path;
      data["abs_path"] = to_generic_string(abs);
      data["structured"] = true;
      data["no_changes"] = true;
      if (promotion) data["promotion"] = promotion_update.report;
      data["previous_revision"] = expected_revision;data["revision"] = transaction.document().revision;
      data["source_id"] = transaction.document().source_id;
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
    wrote_file = structured_entries_changed > 0 || metadata_changed;
  } else {
    wrote_file = true;
  }
  if (wrote_file) {
    const auto keep = args["keep_checkpoints"].isInt() ? std::clamp(args["keep_checkpoints"].asInt(), 2, 1000) : 100;
    store_status = transaction.commit(out_text, &store_error, size_t(keep), promotion ? promotion->publish_observer : std::function<void(MemoryPublishPhase)>{});
    if (store_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_put", store_status, store_error, transaction.document(), expected_revision);
  }

  // Rolling consolidation checkpoint: when a structured memory file is updated, keep a time-stamped
  // JSON snapshot for correlation over time (best-effort; does not fail the tool call).
  bool checkpoint_ok = false, checkpoint_prune_ok = false;
  int checkpoint_pruned_count = 0;
  std::string checkpoint_path_rel;
  std::string checkpoint_ts_utc;
  std::string checkpoint_sha256;
  int64_t checkpoint_bytes = 0;
  try {
  if (wrote_file && structured && structured_doc_present) {
    const bool want_checkpoint =
      args.isMember("checkpoint") && args["checkpoint"].isBool() ? args["checkpoint"].asBool() : true;
    const int keep_checkpoints =
      args.isMember("keep_checkpoints") && args["keep_checkpoints"].isInt() ? std::clamp(args["keep_checkpoints"].asInt(), 1, 1000) : 100;
    if (want_checkpoint) {
      const std::filesystem::path ckdir = mem_root / "checkpoints";
      std::error_code ec2;
      {
        const std::string ts = iso_utc_now();
        checkpoint_ts_utc = ts;
        std::string safe_ts = ts;
        for (char& c : safe_ts) {
          if (c == ':' || c == '/') c = '-';
        }
        const std::filesystem::path ckfile = ckdir / (std::string("structured_") + safe_ts + "_" + transaction.document().source_id.substr(7) + "_" + transaction.document().revision.substr(7) + ".json");

        Json::Value ck(Json::objectValue);
        ck["schema"] = "agent_memory_checkpoint_v1";
        ck["ts_utc"] = ts;
        ck["path"] = rel_path;
        ck["source_id"] = transaction.document().source_id;
        ck["revision"] = transaction.document().revision;
        ck["doc"] = structured_doc_for_checkpoint;
        const std::string ck_text = json_stringify_compact(ck) + "\n";
        checkpoint_bytes = (int64_t)ck_text.size();
        {
          char hex[65];
          agent_sha256_hex_of_bytes(ck_text.data(), ck_text.size(), hex);
          checkpoint_sha256 = std::string(hex);
        }

        std::string checkpoint_error;
        checkpoint_ok = memory_document_publish_immutable(mem_root, ckfile.lexically_relative(mem_root).generic_string(), ck_text, &checkpoint_error);

        if (checkpoint_ok) {
          checkpoint_path_rel = to_generic_string(ckfile.lexically_relative(mem_root));
          // Best-effort pruning: keep only the newest N checkpoint files.
          std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
          checkpoint_prune_ok = true;
          size_t visited = 0;
          const auto prune_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          auto it = std::filesystem::directory_iterator(ckdir, ec2);
          for (; !ec2 && it != std::filesystem::directory_iterator(); it.increment(ec2)) {
            if (++visited > 4096 || is_cancelled(ctx) || std::chrono::steady_clock::now() >= prune_deadline) {
              checkpoint_prune_ok = false;
              break;
            }
            const auto& de = *it;
            if (de.is_symlink(ec2) || !de.is_regular_file(ec2)) continue;
            const std::string fn = de.path().filename().string();
            if (fn.rfind("structured_", 0) != 0) continue;
            if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;
            const auto stamp = de.last_write_time(ec2);
            if (ec2) break;
            entries.emplace_back(stamp, de.path());
          }
          if (ec2) checkpoint_prune_ok = false;
          if (checkpoint_prune_ok && entries.size() > (size_t)keep_checkpoints) {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
              return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            for (size_t i = (size_t)keep_checkpoints; i < entries.size(); i++) {
              if (checkpoint_pruned_count >= 50 || is_cancelled(ctx) || std::chrono::steady_clock::now() >= prune_deadline) {
                checkpoint_prune_ok = false;
                break;
              }
              const auto prune_path = entries[i].second.lexically_relative(mem_root).generic_string();
              MemoryDocument prune_source;
              std::string prune_error;
              if (memory_document_read(mem_root, prune_path, &prune_source, &prune_error) != MemoryStoreStatus::Ok ||
                  transaction.retire_auxiliary(prune_path, prune_source.revision, &prune_error) != MemoryStoreStatus::Ok) {
                checkpoint_prune_ok = false;
                break;
              }
              ++checkpoint_pruned_count;
            }
          }
        }
      }
    }
  }

  } catch (...) { checkpoint_prune_ok = false; /* auxiliary maintenance cannot revoke a committed memory update */ }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_put";
  data["memory_root"] = to_generic_string(mem_root);
  data["path"] = rel_path;
  data["abs_path"] = to_generic_string(abs);
  data["bytes_written"] = Json::UInt64(transaction.published() ? out_text.size() : 0);
  data["previous_revision"] = expected_revision;data["revision"] = transaction.document().revision;
  data["source_id"] = transaction.document().source_id;data["commit_state"] = "committed";
  if (private_bytes_stripped > 0) data["private_bytes_stripped"] = (Json::Int64)private_bytes_stripped;
  if (private_entries_skipped > 0) data["private_entries_skipped"] = private_entries_skipped;
  if (structured) data["structured"] = true;
  if (structured) {
    data["entries_valid"] = structured_entries_valid;
    data["entries_changed"] = structured_entries_changed;
    data["entries_superseded"] = structured_entries_superseded;
    data["entries_evidence_added"] = structured_entries_evidence_added;
    data["checkpoint_ok"] = checkpoint_ok;
    data["checkpoint_prune_ok"] = checkpoint_prune_ok;
    data["checkpoint_pruned_count"] = checkpoint_pruned_count;
    if (!checkpoint_path_rel.empty()) data["checkpoint_path"] = checkpoint_path_rel;
    if (checkpoint_ok) {
      if (!checkpoint_ts_utc.empty()) data["checkpoint_ts_utc"] = checkpoint_ts_utc;
      if (!checkpoint_sha256.empty()) data["checkpoint_sha256"] = checkpoint_sha256;
      if (checkpoint_bytes > 0) data["checkpoint_bytes"] = (Json::Int64)checkpoint_bytes;
    }
  }
  data["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  if (promotion) data["promotion"] = promotion_update.report;
  data["output"] = "memory_put: wrote " + rel_path;
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_put(HostToolCtx* ctx, const char* args, agent_string_t* out) {
  return memory_put_impl(ctx, args, out, nullptr);
}

agent_status_t memory_promote(const HostToolsetConfig& config, const MemoryPromotionPlan& plan,
                             Json::Value* receipt, std::string* error) {
  if (!receipt || !plan.snapshot.sources.count("STRUCTURED.md")) return AGENT_ERR_INVALID_ARGUMENT;
  agent_tool_registry_t* registry = nullptr;
  agent_tool_executor_t executor{};
  auto status = toolset_host_create(config, &registry, &executor);
  if (status != AGENT_OK) { if (error) *error = "failed to create host toolset"; return status; }
  Json::Value args;
  args["path"] = "STRUCTURED.md";
  args["expected_revision"] = plan.snapshot.sources.at("STRUCTURED.md").revision;
  args["entries"] = Json::Value(Json::arrayValue);
  args["keep_checkpoints"] = plan.keep_checkpoints;
  agent_string_t result{};
  status = memory_put_impl(static_cast<HostToolCtx*>(executor.ctx), json_stringify_compact(args).c_str(), &result, &plan);
  const std::string text = result.data ? std::string(result.data, result.len) : "";
  agent_string_free(&result);
  agent_tool_registry_destroy(registry);
  toolset_host_destroy(&executor);
  Json::CharReaderBuilder reader;
  std::istringstream input(text);
  std::string parse_error;
  if (!Json::parseFromStream(reader, input, receipt, &parse_error)) {
    if (error) *error = "invalid native promotion response";
    return AGENT_ERR_INTERNAL;
  }
  if (!(*receipt).get("ok", false).asBool() && error) *error = (*receipt).get("error", "memory promotion failed").asString();
  return status;
}

struct MemoryCandidate {
  std::string abs_path;
  std::string rel_path;
  std::string tier;
};

static std::string memory_relpath(const std::filesystem::path& abs, const std::filesystem::path& root) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
  if (ec) rel = abs.lexically_relative(root);
  return to_generic_string(rel.lexically_normal());
}

static void add_memory_candidate(
  const std::filesystem::path& mem_root,
  const std::filesystem::path& abs,
  const std::string& tier,
  std::vector<MemoryCandidate>* out
) {
  if (!out) return;
  MemoryCandidate c;
  c.abs_path = to_generic_string(abs);
  c.rel_path = memory_relpath(abs, mem_root);
  c.tier = tier;
  out->push_back(std::move(c));
}

static std::vector<MemoryCandidate> list_candidate_memory_files(const HostToolCtx* ctx, int max_days) {
  std::vector<MemoryCandidate> out;
  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) return out;

  const std::filesystem::path core = mem_root / "MEMORY.md";
  const std::filesystem::path structured = mem_root / "STRUCTURED.md";
  std::error_code ec;
  if (std::filesystem::exists(structured, ec) && std::filesystem::is_regular_file(structured, ec)) {
    add_memory_candidate(mem_root, structured, "structured", &out);
  }
  if (std::filesystem::exists(core, ec) && std::filesystem::is_regular_file(core, ec)) {
    add_memory_candidate(mem_root, core, "core", &out);
  }

  // Session layer (optional; only when we have a stable session id).
  const std::string sid = trim_ascii(ctx ? ctx->session_id : "");
  if (!sid.empty()) {
    const std::filesystem::path sessionp = mem_root / "sessions" / (sid + ".md");
    ec.clear();
    if (std::filesystem::exists(sessionp, ec) && std::filesystem::is_regular_file(sessionp, ec)) {
      add_memory_candidate(mem_root, sessionp, "session", &out);
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
      add_memory_candidate(mem_root, p, "daily", &out);
    }
  }
  return out;
}

static bool parse_citation_path_line(const std::string& citation, std::string* out_path, int* out_line) {
  if (out_path) out_path->clear();
  if (out_line) *out_line = 0;
  const std::string c = trim_ascii(citation);
  if (c.empty()) return false;
  const size_t pos = c.rfind(':');
  if (pos == std::string::npos) return false;
  const std::string path = trim_ascii(c.substr(0, pos));
  const std::string line_str = trim_ascii(c.substr(pos + 1));
  if (path.empty() || line_str.empty()) return false;
  int line = 0;
  try {
    line = std::stoi(line_str);
  } catch (...) {
    return false;
  }
  if (line <= 0) return false;
  if (out_path) *out_path = path;
  if (out_line) *out_line = line;
  return true;
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
  const int max_results_raw = args.isMember("max_results") && args["max_results"].isInt() ? args["max_results"].asInt() : 20;
  const int max_results = std::min(std::max(1, max_results_raw), 200);
  const int max_days_raw = args.isMember("daily_days") && args["daily_days"].isInt() ? args["daily_days"].asInt() : 14;
  const int max_days = std::min(std::max(0, max_days_raw), 31);
  const bool case_sensitive =
    args.isMember("case_sensitive") && args["case_sensitive"].isBool() ? args["case_sensitive"].asBool() : false;
  const int context_lines_raw = args.isMember("context_lines") && args["context_lines"].isInt() ? args["context_lines"].asInt() : 2;
  const int context_lines = std::min(std::max(0, context_lines_raw), 20);
  const int max_snippet_chars_raw =
    args.isMember("max_snippet_chars") && args["max_snippet_chars"].isInt() ? args["max_snippet_chars"].asInt() : 600;
  const int max_snippet_chars = std::min(std::max(80, max_snippet_chars_raw), 4000);
  const bool tiered = args.isMember("tiered") && args["tiered"].isBool() ? args["tiered"].asBool() : false;
  std::string order = "ranked";
  if (args.isMember("order") && args["order"].isString()) {
    order = to_lower_ascii(trim_ascii(args["order"].asString()));
    if (order == "newest_first" || order == "latest") order = "newest";
    if (order == "oldest_first" || order == "earliest") order = "oldest";
    if (order != "newest" && order != "oldest") order = "ranked";
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_search requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::vector<MemoryCandidate> candidates = list_candidate_memory_files(ctx, max_days);
  std::vector<std::string> files;
  files.reserve(candidates.size());
  std::unordered_map<std::string, std::string> tier_by_rel;
  for (const auto& c : candidates) {
    files.push_back(c.abs_path);
    if (!c.rel_path.empty() && tier_by_rel.find(c.rel_path) == tier_by_rel.end()) {
      tier_by_rel[c.rel_path] = c.tier;
    }
  }
  const std::string q = case_sensitive ? query : to_lower_ascii(query);

  Json::Value results(Json::arrayValue);
  std::unordered_map<std::string, Json::Value> tier_results;
  std::unordered_map<std::string, int64_t> tier_tokens;
  const std::vector<std::string> tier_order = {"core", "structured", "session", "daily", "other"};
  for (const auto& tier : tier_order) {
    tier_results.emplace(tier, Json::Value(Json::arrayValue));
    tier_tokens[tier] = 0;
  }

  auto estimate_tokens = [](const std::string& snippet) -> int64_t {
    if (snippet.empty()) return 0;
    return (int64_t)((snippet.size() + 3) / 4);
  };
  auto tier_for_rel = [&](const std::string& rel_path) -> std::string {
    auto it = tier_by_rel.find(rel_path);
    if (it != tier_by_rel.end()) return it->second;
    return "other";
  };
  auto record_tier = [&](const std::string& tier, const Json::Value& r) {
    if (!tiered) return;
    auto it = tier_results.find(tier);
    if (it == tier_results.end()) return;
    it->second.append(r);
    const std::string snippet = r.isMember("snippet") && r["snippet"].isString() ? r["snippet"].asString() : "";
    tier_tokens[tier] += estimate_tokens(snippet);
  };

  const bool use_index = args.get("use_index", true).asBool();
  MemoryViewMode view;
  if (!args["view"].isNull() && !args["view"].isString()) return write_envelope(out_result, false, "invalid memory view", Json::Value(Json::objectValue));
  if (!memory_view_mode((args["view"].isNull() ? "current" : args["view"].asString()), &view)) return write_envelope(out_result, false, "view must be current, historical or evidence", Json::Value(Json::objectValue));
  std::string mode, retrieval_error, index_error;
  std::vector<MemorySearchHit> hits;
  if (!memory_retrieve(mem_root, files, query, view, use_index, case_sensitive, max_results, max_snippet_chars, &hits, &mode, &retrieval_error, &index_error, context_lines))
    return write_envelope(out_result, false, retrieval_error, Json::Value(Json::objectValue));
  for (const auto& hit : hits) {
    Json::Value row;
    row["path"] = hit.path;row["line"] = hit.line;row["snippet"] = hit.snippet;row["score"] = hit.score;
    row["tier"] = tier_for_rel(hit.path);row["citation"] = hit.path + ":" + std::to_string(hit.line);
    row["view"] = memory_view_name(view);row["historical"] = hit.passage.historical;
    row["evidence"] = memory_passage_evidence(hit.passage);
    row["supporting_evidence"] = hit.passage.supporting_evidence;
    if (!hit.passage.record_revision.empty()) row["record_revision"] = hit.passage.record_revision;
    if (!hit.passage.eligibility_reason.empty()) row["eligibility_reason"] = hit.passage.eligibility_reason;
    results.append(row);record_tier(row["tier"].asString(), row);
  }
  if (order != "ranked") {
    apply_timeline_order(&results, order);
    if (tiered) {
      auto it = tier_results.find("daily");
      if (it != tier_results.end()) apply_timeline_order(&it->second, order);
    }
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "memory_search";
  data["mode"] = mode;
  data["view"] = memory_view_name(view);
  if (!hits.empty()) data["as_of_utc"] = hits.front().passage.evaluated_utc;
  if (!index_error.empty()) data["index_error"] = index_error;
  data["order"] = order;
  data["memory_root"] = to_generic_string(mem_root);
  data["query"] = query;
  data["files_scanned"] = (Json::Int64)files.size();
  data["results"] = results;
  if (tiered) {
    Json::Value tiers(Json::objectValue);
    for (const auto& tier : tier_order) {
      Json::Value t(Json::objectValue);
      t["results"] = tier_results[tier];
      t["token_estimate"] = (Json::Int64)tier_tokens[tier];
      tiers[tier] = t;
    }
    data["tiers"] = tiers;
  }
  data["output"] = "memory_search mode=" + mode + " order=" + order + " results=" + std::to_string((unsigned)results.size());
  return write_envelope(out_result, true, "", data);
}

agent_status_t tool_memory_timeline(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");

  Json::Value args;
  std::string perr;
  if (!parse_json(arguments_json, &args, &perr) || !args.isObject()) {
    return write_envelope(out_result, false, "invalid args", Json::Value(Json::objectValue));
  }

  std::string rel_path;
  int line = 0;
  const std::string citation = args.isMember("citation") && args["citation"].isString() ? args["citation"].asString() : "";
  if (!citation.empty()) {
    if (!parse_citation_path_line(citation, &rel_path, &line)) {
      return write_envelope(out_result, false, "invalid citation (expected path:line)", Json::Value(Json::objectValue));
    }
  } else {
    rel_path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";
    line = args.isMember("line") && args["line"].isInt() ? args["line"].asInt() : 0;
  }

  if (!is_safe_relpath_md(rel_path) || line <= 0) {
    return write_envelope(out_result, false, "missing/invalid path or line", Json::Value(Json::objectValue));
  }

  const int context_lines_raw =
    args.isMember("context_lines") && args["context_lines"].isInt() ? args["context_lines"].asInt() : 3;
  const int context_lines = std::min(std::max(0, context_lines_raw), 50);
  const int max_chars_raw = args.isMember("max_chars") && args["max_chars"].isInt() ? args["max_chars"].asInt() : 2000;
  const int max_chars = std::min(std::max(200, max_chars_raw), 10000);

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_timeline requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }

  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }

  MemoryViewMode view;
  if (!args["view"].isNull() && !args["view"].isString()) return write_envelope(out_result, false, "invalid memory view", Json::Value(Json::objectValue));
  if (!memory_view_mode((args["view"].isNull() ? "current" : args["view"].asString()), &view)) return write_envelope(out_result, false, "invalid memory view", Json::Value(Json::objectValue));
  MemoryDocument document;std::string read_error;
  if (args.isMember("revision") && !args["revision"].isString()) return write_envelope(out_result, false, "invalid revision", Json::Value(Json::objectValue));
  auto read_status = args.isMember("revision")
    ? memory_document_read_revision(mem_root, rel_path, args["revision"].asString(), &document, &read_error)
    : memory_document_read(mem_root, rel_path, &document, &read_error);
  if (read_status != MemoryStoreStatus::Ok || !document.exists) return write_envelope(out_result, false, read_error.empty() ? "memory source missing" : read_error, Json::Value(Json::objectValue));
  std::vector<MemoryPassage> passages;
  if (!memory_view_project(rel_path, document, view, memory_view_now_utc(), &passages, &read_error)) return write_envelope(out_result, false, read_error, Json::Value(Json::objectValue));
  MemoryViewSnapshot timeline_snapshot;
  if (view == MemoryViewMode::Current) {
    if (!memory_view_capture(mem_root, {abs.string()}, view, &timeline_snapshot, &read_error)) return write_envelope(out_result, false, read_error, Json::Value(Json::objectValue));
    passages = timeline_snapshot.passages;
  }
  const int start_line = std::max(1, line - context_lines);
  const int end_line = line > INT32_MAX - context_lines ? INT32_MAX : line + context_lines;
  int last_included = 0, cur_line = 0;bool saw_line = false;
  std::string text;Json::Value evidence(Json::arrayValue);
  for (const auto& passage : passages) {
    if (passage.end_line < start_line || passage.line > end_line) continue;
    if (passage.line <= line && passage.end_line >= line) saw_line = true;
    if (!passage.pointer.empty()) {
      text += passage.text + "\n";evidence.append(memory_passage_evidence(passage));last_included = passage.line;
    } else {
      std::istringstream input(passage.text);std::string raw;int number = passage.line;size_t offset = passage.begin;
      while (std::getline(input, raw)) {
        const auto count = raw.size() + (offset + raw.size() < passage.end ? 1 : 0);
        if (number >= start_line && number <= end_line) {
          auto slice = passage;slice.begin = offset;slice.end = offset + count;slice.line = number;slice.end_line = number;
          text += raw + "\n";evidence.append(memory_passage_evidence(slice));last_included = number;
        }
        offset += count;++number;
      }
    }
    cur_line = std::max(cur_line, passage.end_line);
  }
  if (!saw_line || text.empty()) return write_envelope(out_result, false, "line is absent or ineligible in this view; use an explicit historical/evidence reference", Json::Value(Json::objectValue));
  if (view == MemoryViewMode::Current && !memory_view_validate(timeline_snapshot, &read_error))
    return write_envelope(out_result, false, read_error, Json::Value(Json::objectValue));
  if (view == MemoryViewMode::Current || !args.isMember("revision")) {
    MemoryDocument current;
    if (memory_document_read(mem_root, rel_path, &current, &read_error) != MemoryStoreStatus::Ok || current.revision != document.revision)
      return write_envelope(out_result, false, "memory changed during timeline retrieval", Json::Value(Json::objectValue));
  }
  const bool truncated = text.size() > size_t(max_chars);
  text = memory_view_clip(text, size_t(max_chars));
  Json::Value data(Json::objectValue);
  data["tool"] = "memory_timeline";
  data["view"] = memory_view_name(view);data["revision"] = document.revision;data["source_id"] = document.source_id;data["evidence"] = evidence;
  data["path"] = rel_path;
  data["line"] = line;
  data["start_line"] = start_line;
  data["end_line"] = last_included > 0 ? last_included : std::min(end_line, cur_line);
  data["context_lines"] = context_lines;
  data["citation"] = rel_path + ":" + std::to_string(line);
  data["memory_root"] = to_generic_string(mem_root);
  data["text"] = text;
  data["truncated"] = truncated;
  data["output"] = "memory_timeline: " + data["citation"].asString();
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
  const std::string updated_since_utc =
    args.isMember("updated_since_utc") && args["updated_since_utc"].isString() ? trim_ascii(args["updated_since_utc"].asString()) : "";
  const std::string updated_until_utc =
    args.isMember("updated_until_utc") && args["updated_until_utc"].isString() ? trim_ascii(args["updated_until_utc"].asString()) : "";
  const std::string observed_since_utc =
    args.isMember("observed_since_utc") && args["observed_since_utc"].isString() ? trim_ascii(args["observed_since_utc"].asString()) : "";
  const std::string observed_until_utc =
    args.isMember("observed_until_utc") && args["observed_until_utc"].isString() ? trim_ascii(args["observed_until_utc"].asString()) : "";
  const std::string valid_from_since_utc =
    args.isMember("valid_from_since_utc") && args["valid_from_since_utc"].isString() ? trim_ascii(args["valid_from_since_utc"].asString()) : "";
  const std::string valid_from_until_utc =
    args.isMember("valid_from_until_utc") && args["valid_from_until_utc"].isString() ? trim_ascii(args["valid_from_until_utc"].asString()) : "";
  const std::string order_by =
    args.isMember("order_by") && args["order_by"].isString() ? to_lower_ascii(trim_ascii(args["order_by"].asString())) : "key_asc";

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

  MemoryViewMode view;
  if (!args["view"].isNull() && !args["view"].isString()) return write_envelope(out_result, false, "invalid memory view", Json::Value(Json::objectValue));
  if (!memory_view_mode((args["view"].isNull() ? "current" : args["view"].asString()), &view)) return write_envelope(out_result, false, "invalid memory view", Json::Value(Json::objectValue));
  const std::string status_filter_raw =
    args.isMember("status") && args["status"].isString() ? to_lower_ascii(trim_ascii(args["status"].asString())) : (view == MemoryViewMode::Current ? "active" : "any");
  const std::string status_filter = status_filter_raw.empty() ? "active" : status_filter_raw;

  const bool include_sources =
    args.isMember("include_sources") && args["include_sources"].isBool() ? args["include_sources"].asBool() : true;
  const bool include_versions =
    args.isMember("include_versions") && args["include_versions"].isBool() ? args["include_versions"].asBool() : false;

  if (view == MemoryViewMode::Current && include_versions)
    return write_envelope(out_result, false, "include_versions requires historical or evidence view", Json::Value(Json::objectValue));
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

  if (!is_iso_utc_string(updated_since_utc)) {
    return write_envelope(out_result, false, "updated_since_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!is_iso_utc_string(updated_until_utc)) {
    return write_envelope(out_result, false, "updated_until_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!updated_since_utc.empty() && !updated_until_utc.empty() && updated_until_utc < updated_since_utc) {
    return write_envelope(out_result, false, "updated_until_utc must be >= updated_since_utc", Json::Value(Json::objectValue));
  }
  if (!is_iso_utc_string(observed_since_utc)) {
    return write_envelope(out_result, false, "observed_since_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!is_iso_utc_string(observed_until_utc)) {
    return write_envelope(out_result, false, "observed_until_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!observed_since_utc.empty() && !observed_until_utc.empty() && observed_until_utc < observed_since_utc) {
    return write_envelope(out_result, false, "observed_until_utc must be >= observed_since_utc", Json::Value(Json::objectValue));
  }
  if (!is_iso_utc_string(valid_from_since_utc)) {
    return write_envelope(out_result, false, "valid_from_since_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!is_iso_utc_string(valid_from_until_utc)) {
    return write_envelope(out_result, false, "valid_from_until_utc must be ISO UTC like 2026-02-05T00:00:00Z", Json::Value(Json::objectValue));
  }
  if (!valid_from_since_utc.empty() && !valid_from_until_utc.empty() && valid_from_until_utc < valid_from_since_utc) {
    return write_envelope(out_result, false, "valid_from_until_utc must be >= valid_from_since_utc", Json::Value(Json::objectValue));
  }
  if (order_by != "key_asc" && order_by != "updated_desc" && order_by != "observed_desc" && order_by != "valid_from_desc") {
    return write_envelope(out_result, false, "order_by must be key_asc, updated_desc, observed_desc, or valid_from_desc", Json::Value(Json::objectValue));
  }

  const std::filesystem::path mem_root = memory_root_from_ctx(ctx);
  if (mem_root.empty()) {
    return write_envelope(out_result, false, "memory_structured_query requires daemon session context (sessions_root_dir)", Json::Value(Json::objectValue));
  }
  const std::filesystem::path abs = (mem_root / rel_path).lexically_normal();
  if (!path_is_within(mem_root.lexically_normal(), abs)) {
    return write_envelope(out_result, false, "invalid path", Json::Value(Json::objectValue));
  }

  MemoryDocument snapshot;std::string snapshot_error;
  const auto snapshot_status = memory_document_read(mem_root, rel_path, &snapshot, &snapshot_error);
  if (snapshot_status != MemoryStoreStatus::Ok) return memory_store_error(out_result, "memory_structured_query", snapshot_status, snapshot_error);
  const bool missing = !snapshot.exists;
  const auto& content = snapshot.text;

  Json::Value doc;
  std::string derr;
  if (!(view == MemoryViewMode::Current ? memory_structured_parse_doc(content, &doc, &derr)
                                        : memory_structured_decode(content, &doc, &derr))) {
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

  struct Match {
    MemoryPassage passage;
    std::string key;
    Json::Value rec;
    std::string updated_utc;
    std::string observed_utc;
    std::string valid_from;
  };
  std::vector<Match> matches;
  matches.reserve(128);

  MemoryViewSnapshot query_snapshot;
  if (!memory_view_capture(mem_root, {abs.string()}, view, &query_snapshot, &derr))
    return write_envelope(out_result, false, derr, Json::Value(Json::objectValue));
  const auto captured_source = query_snapshot.sources.find(rel_path);
  if (captured_source == query_snapshot.sources.end() || captured_source->second.revision != snapshot.revision)
    return write_envelope(out_result, false, "memory changed or source path is not normalized", Json::Value(Json::objectValue));
  const auto as_of = query_snapshot.as_of_utc;
  for (const auto& passage : query_snapshot.passages) {
    if (passage.path != rel_path || passage.pointer.empty() || (!include_versions && passage.pointer.find("/versions/") != std::string::npos)) continue;
    const auto& k = passage.key;

    if (!key_n.empty()) {
      if (key_norm(k) != key_n) continue;
    } else if (!prefix_match(k)) {
      continue;
    }
    const Json::Value rec = passage.record;
    if (!rec.isObject()) continue;
    const std::string kind = rec.isMember("kind") && rec["kind"].isString() ? rec["kind"].asString() : "fact";
    const std::string status = rec.isMember("status") && rec["status"].isString() ? rec["status"].asString() : "active";
    if (!kind_match(kind)) continue;
    if (!status_match(status)) continue;
    if (!sources_match(rec)) continue;
    const std::string upd = rec.isMember("updated_utc") && rec["updated_utc"].isString() ? rec["updated_utc"].asString() : "";
    if (!updated_since_utc.empty() && (upd.empty() || upd < updated_since_utc)) continue;
    if (!updated_until_utc.empty() && (upd.empty() || upd > updated_until_utc)) continue;
    const std::string obs = rec.isMember("observed_utc") && rec["observed_utc"].isString() ? rec["observed_utc"].asString() : "";
    if (!observed_since_utc.empty() && (obs.empty() || obs < observed_since_utc)) continue;
    if (!observed_until_utc.empty() && (obs.empty() || obs > observed_until_utc)) continue;
    const std::string vf = rec.isMember("valid_from") && rec["valid_from"].isString() ? rec["valid_from"].asString() : "";
    if (!valid_from_since_utc.empty() && (vf.empty() || vf < valid_from_since_utc)) continue;
    if (!valid_from_until_utc.empty() && (vf.empty() || vf > valid_from_until_utc)) continue;

    Match m;
    m.key = k;
    m.passage = passage;
    m.rec = rec;
    m.updated_utc = upd;
    m.observed_utc = obs;
    m.valid_from = vf;
    matches.push_back(std::move(m));


  }

  if (order_by == "updated_desc") {
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
      if (a.updated_utc != b.updated_utc) return a.updated_utc > b.updated_utc;
      return a.key != b.key ? a.key < b.key : a.passage.pointer < b.passage.pointer;
    });
  } else if (order_by == "observed_desc") {
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
      if (a.observed_utc != b.observed_utc) return a.observed_utc > b.observed_utc;
      return a.key != b.key ? a.key < b.key : a.passage.pointer < b.passage.pointer;
    });
  } else if (order_by == "valid_from_desc") {
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
      if (a.valid_from != b.valid_from) return a.valid_from > b.valid_from;
      return a.key != b.key ? a.key < b.key : a.passage.pointer < b.passage.pointer;
    });
  } else {
    std::sort(matches.begin(), matches.end(), [&](const Match& a, const Match& b) {
      if (key_case_insensitive) return to_lower_ascii(a.key) < to_lower_ascii(b.key);
      return a.key != b.key ? a.key < b.key : a.passage.pointer < b.passage.pointer;
    });
  }

  Json::Value results(Json::arrayValue);
  int matched = 0;
  for (size_t i = 0; i < matches.size() && (int)results.size() < limit; i++) {
    const Match& m = matches[i];
    Json::Value out_rec = m.rec;
    if (!include_versions && out_rec.isMember("versions")) out_rec.removeMember("versions");
    if (!include_sources && out_rec.isMember("sources")) out_rec.removeMember("sources");

    Json::Value row(Json::objectValue);
    row["key"] = m.key;
    row["record"] = out_rec;
    row["historical"] = m.passage.historical;row["evidence"] = memory_passage_evidence(m.passage);
    row["record_revision"] = m.passage.record_revision;
    row["supporting_evidence"] = m.passage.supporting_evidence;
    if (!m.passage.eligibility_reason.empty()) row["eligibility_reason"] = m.passage.eligibility_reason;
    results.append(row);
    matched++;
  }

  MemoryDocument current;
  if (!memory_view_validate(query_snapshot, &derr) || memory_document_read(mem_root, rel_path, &current, &derr) != MemoryStoreStatus::Ok || current.revision != snapshot.revision)
    return write_envelope(out_result, false, "memory changed during structured retrieval; reread", Json::Value(Json::objectValue));
  Json::Value data(Json::objectValue);
  data["tool"] = "memory_structured_query";
  data["view"] = memory_view_name(view);
  data["as_of_utc"] = as_of;
  data["revision"] = snapshot.revision;data["source_id"] = snapshot.source_id;
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
  if (!updated_since_utc.empty()) q["updated_since_utc"] = updated_since_utc;
  if (!updated_until_utc.empty()) q["updated_until_utc"] = updated_until_utc;
  if (!observed_since_utc.empty()) q["observed_since_utc"] = observed_since_utc;
  if (!observed_until_utc.empty()) q["observed_until_utc"] = observed_until_utc;
  if (!valid_from_since_utc.empty()) q["valid_from_since_utc"] = valid_from_since_utc;
  if (!valid_from_until_utc.empty()) q["valid_from_until_utc"] = valid_from_until_utc;
  q["order_by"] = order_by.empty() ? "key_asc" : order_by;
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
