#include "memory_correlation_index.h"
#include "memory_store.h"
#include "memory_structured.h"
#include "memory_view.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
namespace agentd {
namespace {
using namespace host_tools_internal;
std::string date_days_ago(int days) {
  auto point = std::chrono::system_clock::now() - std::chrono::hours(24LL * days);
  const auto time = std::chrono::system_clock::to_time_t(point);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  char date[32]{};
  std::strftime(date, sizeof(date), "%Y-%m-%d.md", &tm);
  return date;
}
void tokens(const std::string &text, std::set<std::string> *out) {
  std::istringstream input(text);
  std::string word;
  while (input >> word) {
    const auto start = word.find("trace:");
    if (start == std::string::npos)
      continue;
    if (start > 0 && word[start - 1] != '"' && word[start - 1] != '\'' && word[start - 1] != '[' &&
        word[start - 1] != '(')
      continue;
    word = word.substr(start);
    while (!word.empty() && std::string(",;.\"')]").find(word.back()) != std::string::npos)
      word.pop_back();
    if (word.size() > 6 && word.size() <= 512)
      out->insert(word);
  }
}
std::set<std::string> passage_tokens(const MemoryPassage &p) {
  std::set<std::string> out;
  if (!p.pointer.empty()) {
    if (p.record["sources"].isArray())
      for (const auto &source : p.record["sources"])
        if (source.isString())
          tokens(source.asString(), &out);
  } else {
    tokens(p.text, &out);
    if (p.record["trace_id"].isString())
      tokens("trace:" + p.record["trace_id"].asString(), &out);
  }
  return out;
}
bool matches(const std::set<std::string> &source, const std::string &token) {
  for (const auto &value : source)
    if (value == token || value.rfind(token + ":", 0) == 0)
      return true;
  return false;
}
bool capture(const std::filesystem::path &root, const MemoryCorrelationIndexOptions &options,
             MemoryViewSnapshot *snapshot, std::string *error,
             const std::string &structured_path = "") {
  std::vector<std::string> files;
  if (options.include_structured)
    files.push_back(
        (root / (structured_path.empty() ? "STRUCTURED.md" : structured_path)).string());
  if (options.include_daily) {
    files.push_back((root / "MEMORY.md").string());
    for (int day = 0; day < std::clamp(options.daily_days, 0, 90); ++day)
      files.push_back((root / date_days_ago(day)).string());
  }
  return memory_view_capture(root, files, MemoryViewMode::Current, snapshot, error);
}
Json::Value display_record(const MemoryPassage &p) {
  Json::Value out;
  for (const auto *name : {"kind", "status", "updated_utc", "observed_utc", "valid_from"})
    if (p.record[name].isString())
      out[name] = memory_view_clip(p.record[name].asString(), 160);
  if (p.record["sources"].isArray()) {
    out["sources"] = Json::Value(Json::arrayValue);
    for (const auto &source : p.record["sources"]) {
      if (out["sources"].size() >= 32) {
        out["sources_truncated"] = true;
        break;
      }
      if (source.isString()) {
        out["sources"].append(memory_view_clip(source.asString(), 512));
        if (source.asString().size() > 512)
          out["sources_truncated"] = true;
      }
    }
  }
  const auto &value = p.record["value"];
  if (value.isString()) {
    out["value"] = memory_view_clip(value.asString(), 160);
    out["value_truncated"] = value.asString().size() > 160;
  } else {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const auto text = Json::writeString(writer, value);
    out["value"] = text.size() > 160 ? Json::Value(memory_view_clip(text, 160)) : value;
    out["value_truncated"] = text.size() > 160;
  }
  return out;
}
} // namespace
bool memory_correlation_source_matches(const std::string &source, const std::string &token) {
  std::set<std::string> found;
  tokens(source, &found);
  return matches(found, token);
}
MemoryCorrelationIndexOptions memory_correlation_index_default_options() { return {}; }
bool memory_correlation_index_build(const std::filesystem::path &root,
                                    const MemoryCorrelationIndexOptions &options,
                                    MemoryCorrelationIndexReport *report, std::string *error) {
  if (error)
    error->clear();
  if (!report || root.empty())
    return false;
  *report = {};
  MemoryCacheLease lease;
  if (lease.begin(root, {}, error) != MemoryStoreStatus::Ok || !lease.discard_obsolete(error))
    return false;
  MemoryViewSnapshot snapshot;
  if (!capture(root, options, &snapshot, error))
    return false;
  std::set<std::string> all;
  for (const auto &p : snapshot.passages) {
    const auto found = passage_tokens(p);
    if (found.empty())
      continue;
    all.insert(found.begin(), found.end());
    ++report->entry_count;
    if (p.pointer.empty())
      ++report->daily_entries;
    else
      ++report->structured_entries;
  }
  report->token_count = int(all.size());
  report->generated_utc = snapshot.as_of_utc;
  report->generated_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  return memory_view_validate(snapshot, error);
}
bool memory_correlation_index_query(const std::filesystem::path &root, const std::string &token,
                                    int limit, Json::Value *entries, Json::Value *metadata,
                                    std::string *error, const std::string &structured_path,
                                    const std::string &key_prefix) {
  if (error)
    error->clear();
  if (!entries || !metadata || root.empty() || token.size() <= 6 || token.size() > 512 ||
      token.rfind("trace:", 0) != 0) {
    if (error)
      *error = "invalid correlation token";
    return false;
  }
  *entries = Json::Value(Json::arrayValue);
  *metadata = Json::Value(Json::objectValue);
  MemoryCacheLease lease;
  if (lease.begin(root, {}, error) != MemoryStoreStatus::Ok || !lease.discard_obsolete(error))
    return false;
  MemoryViewSnapshot snapshot;
  MemoryCorrelationIndexOptions options;
  if (!structured_path.empty())
    options.include_daily = false;
  if (!capture(root, options, &snapshot, error, structured_path))
    return false;
  limit = std::clamp(limit, 1, 2000);
  int matched = 0;
  for (const auto &p : snapshot.passages) {
    if ((!key_prefix.empty() && (p.pointer.empty() || p.key.rfind(key_prefix, 0) != 0)) ||
        !matches(passage_tokens(p), token))
      continue;
    ++matched;
    if (int(entries->size()) >= limit)
      continue;
    Json::Value row;
    row["kind"] = p.pointer.empty() ? "daily" : "structured";
    row["ref"] = memory_passage_evidence(p)["id"];
    row["path"] = p.path;
    row["line"] = p.line;
    row["historical"] = false;
    row["evidence"] = memory_passage_evidence(p);
    row["supporting_evidence"] = p.supporting_evidence;
    if (!p.pointer.empty()) {
      row["key"] = p.key;
      row["structured_path"] = p.path;
      row["record"] = display_record(p);
      row["record_revision"] = p.record_revision;
      auto relationship = p;
      relationship.pointer =
          relationship.pointer.substr(0, relationship.pointer.size() - 6) + "/sources";
      row["relationship_evidence"] = memory_passage_evidence(relationship);
    } else {
      row["text"] = memory_view_clip(p.text, 160);
      row["text_truncated"] = p.text.size() > 160;
      row["trace_id"] = token.substr(6);
    }
    entries->append(row);
  }
  (*metadata)["schema"] = "agent.memory.correlation.v2";
  (*metadata)["engine"] = "live_projection";
  (*metadata)["view"] = "current";
  (*metadata)["as_of_utc"] = snapshot.as_of_utc;
  (*metadata)["matched"] = matched;
  (*metadata)["incomplete"] = matched > limit;
  (*metadata)["input_revisions"] = Json::Value(Json::arrayValue);
  for (const auto &source : snapshot.sources) {
    Json::Value input;
    input["path"] = source.first;
    input["revision"] = source.second.revision;
    input["source_id"] = source.second.source_id;
    (*metadata)["input_revisions"].append(input);
  }
  if (!memory_view_validate(snapshot, error)) {
    *entries = Json::Value(Json::arrayValue);
    return false;
  }
  return true;
}
} // namespace agentd
