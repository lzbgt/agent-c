#include "memory_recaps.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string iso_utc_now() {
  const auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

std::string sanitize_ts_for_path(std::string ts) {
  for (char& c : ts) {
    if (c == ':' || c == '/') c = '-';
  }
  return ts;
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

bool status_is_inactive(const std::string& status) {
  const std::string s = to_lower_ascii(trim_copy(status));
  return s == "deprecated" || s == "inactive" || s == "obsolete" || s == "disabled";
}

std::string safe_item_source(const MemorySalienceItem& item) {
  if (item.tier == "daily") {
    std::ostringstream oss;
    if (!item.path.empty()) oss << item.path;
    if (item.line > 0) oss << "#L" << item.line;
    return oss.str();
  }
  if (!item.key.empty()) return std::string("structured:") + item.key;
  return "";
}

std::string truncate_ascii(std::string s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  if (max_chars < 3) return s.substr(0, max_chars);
  s.resize(max_chars - 3);
  s += "...";
  return s;
}

std::string build_recap_prompt(const MemorySalienceReport& rep, bool* out_truncated) {
  if (out_truncated) *out_truncated = false;
  const size_t max_prompt_chars = 12000;
  std::ostringstream oss;
  oss << "Memory items (ranked by recency + importance). Summarize only what appears below.\n\n";

  if (!rep.structured_items.empty()) {
    oss << "[structured]\n";
    for (const auto& item : rep.structured_items) {
      oss << "- key: " << item.key << "\n";
      if (!item.kind.empty() || !item.status.empty() || !item.ts_utc.empty()) {
        oss << "  meta: kind=" << (item.kind.empty() ? "fact" : item.kind)
            << " status=" << (item.status.empty() ? "active" : item.status);
        if (!item.ts_utc.empty()) oss << " ts_utc=" << item.ts_utc;
        if (status_is_inactive(item.status)) oss << " inactive=true";
        oss << "\n";
      }
      if (!item.text.empty()) {
        oss << "  value: " << item.text << "\n";
      }
    }
    oss << "\n";
  }

  if (!rep.daily_items.empty()) {
    oss << "[daily]\n";
    for (const auto& item : rep.daily_items) {
      oss << "- source: " << safe_item_source(item) << "\n";
      if (!item.ts_utc.empty() || item.importance >= 0) {
        oss << "  meta: ";
        if (!item.ts_utc.empty()) oss << "ts_utc=" << item.ts_utc << " ";
        if (item.importance >= 0) oss << "importance=" << item.importance << " ";
        oss << "\n";
      }
      if (!item.text.empty()) {
        oss << "  text: " << item.text << "\n";
      }
    }
  }

  std::string out = oss.str();
  if (out.size() > max_prompt_chars) {
    out.resize(max_prompt_chars);
    if (out_truncated) *out_truncated = true;
  }
  return out;
}

Json::Value salience_policy_to_json(const MemorySaliencePolicy& pol) {
  Json::Value p(Json::objectValue);
  p["include_structured"] = pol.include_structured;
  p["include_daily"] = pol.include_daily;
  p["daily_days"] = pol.daily_days;
  p["max_items"] = pol.max_items;
  p["max_structured_items"] = pol.max_structured_items;
  p["max_daily_items"] = pol.max_daily_items;
  p["half_life_days"] = pol.half_life_days;
  p["importance_weight"] = pol.importance_weight;
  return p;
}

Json::Value salience_items_to_json(const std::vector<MemorySalienceItem>& items) {
  Json::Value arr(Json::arrayValue);
  for (const auto& item : items) {
    Json::Value row(Json::objectValue);
    row["tier"] = item.tier;
    if (!item.key.empty()) row["key"] = item.key;
    if (!item.kind.empty()) row["kind"] = item.kind;
    if (!item.status.empty()) row["status"] = item.status;
    if (!item.ts_utc.empty()) row["ts_utc"] = item.ts_utc;
    if (!item.path.empty()) row["path"] = item.path;
    if (item.line > 0) row["line"] = item.line;
    if (item.importance >= 0) row["importance"] = item.importance;
    row["score"] = item.score;
    row["text"] = item.text;
    row["source"] = safe_item_source(item);
    arr.append(row);
  }
  return arr;
}

bool read_file_bounded(const std::filesystem::path& p, size_t max_bytes, std::string* out) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  const uintmax_t sz = std::filesystem::file_size(p, ec);
  if (ec) return false;
  if (sz > max_bytes) return false;
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

}  // namespace

bool memory_generate_recap(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const MemoryRecapOptions& opt,
  MemoryRecapReport* out_report,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_report) {
    if (out_err) *out_err = "missing output";
    return false;
  }
  *out_report = MemoryRecapReport();

  if (cfg.state_dir.empty()) {
    if (out_err) *out_err = "state_dir not configured";
    return false;
  }
  const std::filesystem::path mem_root = (std::filesystem::path(cfg.state_dir) / "memory").lexically_normal();
  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    if (out_err) *out_err = "memory root not found";
    return false;
  }

  MemorySalienceReport sal;
  std::string serr;
  if (!memory_salience_collect(mem_root, opt.salience, &sal, &serr)) {
    if (out_err) *out_err = serr.empty() ? "memory salience failed" : serr;
    return false;
  }

  MemoryRecapReport rep;
  rep.generated_utc_ms = sal.generated_utc_ms > 0 ? sal.generated_utc_ms : now_utc_ms();
  rep.ts_utc = iso_utc_now();
  rep.model = trim_copy(opt.model);
  rep.dry_run = opt.dry_run;
  rep.write_file = opt.write_file;
  rep.policy = opt.salience;
  rep.salience = sal;

  bool prompt_truncated = false;
  rep.prompt = build_recap_prompt(sal, &prompt_truncated);
  rep.prompt_truncated = prompt_truncated;

  if (opt.dry_run) {
    *out_report = rep;
    return true;
  }

  if (rep.model.empty()) {
    if (out_err) *out_err = "missing recap model (set summary_model or pass model)";
    return false;
  }
  if (rep.prompt.empty()) {
    if (out_err) *out_err = "recap prompt is empty";
    return false;
  }

  OpenAIClientConfig run_cfg = ocfg;
  run_cfg.model = rep.model;

  const std::string system =
    "You are a memory recap assistant.\n"
    "Summarize the memory items into a structured recap.\n"
    "Return JSON ONLY with keys:\n"
    "  request, investigated, learned, completed, next_steps, files_read, files_modified, notes.\n"
    "Use strings for request/investigated/learned/completed/notes.\n"
    "Use arrays of strings for next_steps, files_read, files_modified.\n"
    "If information is missing, use empty strings/arrays.\n"
    "Do not invent facts.\n";

  std::string user;
  user += "Memory items (may be truncated):\n\n";
  user += rep.prompt;
  user += "\n\nReturn compact JSON only. No markdown or code fences.\n";

  agent_message_view_t msgs[2]{};
  msgs[0].role = AGENT_ROLE_SYSTEM;
  msgs[0].content = system.c_str();
  msgs[0].content_len = system.size();
  msgs[1].role = AGENT_ROLE_USER;
  msgs[1].content = user.c_str();
  msgs[1].content_len = user.size();

  const OpenAIChatResult r = openai_chat_completions(run_cfg, msgs, 2);
  if (r.http_status < 200 || r.http_status >= 300) {
    if (out_err) {
      *out_err = !r.error_message.empty() ? r.error_message : openai_format_http_error(r.http_status, r.response_body);
    }
    return false;
  }

  std::string summary = trim_copy(r.assistant_text);
  if (summary.empty()) {
    if (out_err) *out_err = "recap model returned empty summary";
    return false;
  }
  if (opt.summary_max_chars > 0 && summary.size() > opt.summary_max_chars) {
    summary = truncate_ascii(summary, opt.summary_max_chars);
  }
  rep.summary_text = summary;

  Json::Value parsed(Json::nullValue);
  std::string perr;
  if (json_parse_any(summary, &parsed, &perr) && parsed.isObject()) {
    rep.summary_json_ok = true;
    rep.summary_json = parsed;
  } else {
    rep.summary_json_ok = false;
  }

  if (opt.write_file) {
    const std::filesystem::path recap_dir = mem_root / "recaps";
    std::filesystem::create_directories(recap_dir, ec);
    if (ec) {
      if (out_err) *out_err = "failed to create recaps dir";
      return false;
    }
    const std::string safe_ts = sanitize_ts_for_path(rep.ts_utc);
    const std::filesystem::path recap_path = recap_dir / (std::string("recap_") + safe_ts + ".json");

    Json::Value doc(Json::objectValue);
    doc["schema"] = "agentd_memory_recap_v1";
    doc["ts_utc"] = rep.ts_utc;
    doc["ts_utc_ms"] = (Json::Int64)rep.generated_utc_ms;
    doc["model"] = rep.model;
    doc["summary_max_chars"] = (Json::Int64)opt.summary_max_chars;
    doc["policy"] = salience_policy_to_json(rep.policy);
    Json::Value input(Json::objectValue);
    input["structured_count"] = (Json::Int64)rep.salience.structured_items.size();
    input["daily_count"] = (Json::Int64)rep.salience.daily_items.size();
    input["total_count"] = (Json::Int64)(rep.salience.structured_items.size() + rep.salience.daily_items.size());
    doc["input"] = input;
    doc["summary_text"] = rep.summary_text;
    if (rep.summary_json_ok) doc["summary"] = rep.summary_json;
    doc["structured_items"] = salience_items_to_json(rep.salience.structured_items);
    doc["daily_items"] = salience_items_to_json(rep.salience.daily_items);

    const std::string text = json_stringify(doc);
    std::ofstream out(recap_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      if (out_err) *out_err = "failed to write recap file";
      return false;
    }
    out.write(text.data(), (std::streamsize)text.size());
    if (!out.good()) {
      if (out_err) *out_err = "failed to write recap file";
      return false;
    }
    rep.recap_path_rel = recap_path.lexically_relative(mem_root).generic_string();
    rep.recap_bytes = (int64_t)text.size();
  }

  *out_report = rep;
  return true;
}

bool memory_list_recaps(
  const DaemonConfig& cfg,
  int limit,
  bool include_summary,
  Json::Value* out_list,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_list) {
    if (out_err) *out_err = "missing output";
    return false;
  }
  *out_list = Json::Value(Json::arrayValue);

  if (cfg.state_dir.empty()) {
    if (out_err) *out_err = "state_dir not configured";
    return false;
  }
  const std::filesystem::path mem_root = (std::filesystem::path(cfg.state_dir) / "memory").lexically_normal();
  const std::filesystem::path recap_dir = mem_root / "recaps";
  std::error_code ec;
  if (!std::filesystem::exists(recap_dir, ec) || !std::filesystem::is_directory(recap_dir, ec)) {
    return true;
  }

  struct RecapRow {
    std::filesystem::path path;
    std::string rel;
    int64_t ts_ms = 0;
    int64_t bytes = 0;
    std::string ts_utc;
    std::string model;
    std::string summary_text;
  };
  std::vector<RecapRow> rows;

  for (auto it = std::filesystem::directory_iterator(recap_dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
    const auto& de = *it;
    if (!de.is_regular_file(ec)) continue;
    const std::string fn = de.path().filename().string();
    if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;
    const auto sz = std::filesystem::file_size(de.path(), ec);
    if (ec) continue;
    std::string text;
    if (!read_file_bounded(de.path(), 1024 * 1024, &text)) continue;
    Json::Value doc;
    std::string perr;
    if (!json_parse_any(text, &doc, &perr) || !doc.isObject()) continue;

    RecapRow row;
    row.path = de.path();
    row.rel = de.path().lexically_relative(mem_root).generic_string();
    row.bytes = (int64_t)sz;
    if (doc.isMember("ts_utc") && doc["ts_utc"].isString()) row.ts_utc = doc["ts_utc"].asString();
    if (doc.isMember("ts_utc_ms") && doc["ts_utc_ms"].isInt64()) row.ts_ms = doc["ts_utc_ms"].asInt64();
    if (row.ts_ms <= 0) {
      const auto mtime = de.last_write_time(ec);
      if (!ec) {
        row.ts_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
          mtime.time_since_epoch()).count();
      }
    }
    if (doc.isMember("model") && doc["model"].isString()) row.model = doc["model"].asString();
    if (include_summary && doc.isMember("summary_text") && doc["summary_text"].isString()) {
      row.summary_text = doc["summary_text"].asString();
    }
    rows.push_back(std::move(row));
  }

  std::sort(rows.begin(), rows.end(), [](const RecapRow& a, const RecapRow& b) {
    if (a.ts_ms == b.ts_ms) return a.rel < b.rel;
    return a.ts_ms > b.ts_ms;
  });

  const int lim = std::max(1, std::min(200, limit));
  for (size_t i = 0; i < rows.size() && (int)i < lim; i++) {
    const auto& row = rows[i];
    Json::Value o(Json::objectValue);
    o["recap_path"] = row.rel;
    o["bytes"] = (Json::Int64)row.bytes;
    if (!row.ts_utc.empty()) o["ts_utc"] = row.ts_utc;
    if (row.ts_ms > 0) o["ts_utc_ms"] = (Json::Int64)row.ts_ms;
    if (!row.model.empty()) o["model"] = row.model;
    if (include_summary && !row.summary_text.empty()) o["summary_text"] = row.summary_text;
    out_list->append(o);
  }

  return true;
}

Json::Value memory_recap_report_to_json(const MemoryRecapReport& rep, bool include_prompt) {
  Json::Value o(Json::objectValue);
  o["generated_utc_ms"] = (Json::Int64)rep.generated_utc_ms;
  if (!rep.ts_utc.empty()) o["ts_utc"] = rep.ts_utc;
  if (!rep.model.empty()) o["model"] = rep.model;
  o["dry_run"] = rep.dry_run;
  o["write_file"] = rep.write_file;
  o["policy"] = salience_policy_to_json(rep.policy);
  Json::Value input(Json::objectValue);
  input["structured_count"] = (Json::Int64)rep.salience.structured_items.size();
  input["daily_count"] = (Json::Int64)rep.salience.daily_items.size();
  input["total_count"] = (Json::Int64)(rep.salience.structured_items.size() + rep.salience.daily_items.size());
  o["input"] = input;
  o["structured_items"] = salience_items_to_json(rep.salience.structured_items);
  o["daily_items"] = salience_items_to_json(rep.salience.daily_items);
  if (include_prompt && !rep.prompt.empty()) {
    o["prompt"] = rep.prompt;
    o["prompt_truncated"] = rep.prompt_truncated;
  }
  if (!rep.summary_text.empty()) o["summary_text"] = rep.summary_text;
  if (rep.summary_json_ok) o["summary"] = rep.summary_json;
  if (!rep.recap_path_rel.empty()) o["recap_path"] = rep.recap_path_rel;
  if (rep.recap_bytes > 0) o["recap_bytes"] = (Json::Int64)rep.recap_bytes;
  return o;
}

}  // namespace agentd
