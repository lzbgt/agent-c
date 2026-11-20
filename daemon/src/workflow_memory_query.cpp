#include "workflow_memory_query.h"

#include "json_util.h"
#include "memory_checkpoints.h"
#include "memory_view.h"
#include "memory_structured.h"
#include <sstream>
#include "string_util.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agentd {
namespace {

static std::filesystem::path memory_root_from_state_dir(const std::string& state_dir) {
  if (state_dir.empty()) return {};
  return (std::filesystem::path(state_dir) / "memory").lexically_normal();
}

static Json::Value err_out(const std::string& err) {
  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["assistant_text"] = "";
  out["error"] = err;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  return out;
}

}  // namespace

Json::Value workflow_memory_query_to_json(
  const std::string& state_dir,
  const Json::Value& memory_query,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  if (!memory_query.isObject()) {
    if (out_err) *out_err = "memory_query must be an object";
    return err_out("memory_query must be an object");
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (memory_query.isMember("since_utc_ms") && (memory_query["since_utc_ms"].isInt64() || memory_query["since_utc_ms"].isUInt64() || memory_query["since_utc_ms"].isInt() || memory_query["since_utc_ms"].isUInt())) {
    since_ms = memory_query["since_utc_ms"].asInt64();
  }
  if (memory_query.isMember("until_utc_ms") && (memory_query["until_utc_ms"].isInt64() || memory_query["until_utc_ms"].isUInt64() || memory_query["until_utc_ms"].isInt() || memory_query["until_utc_ms"].isUInt())) {
    until_ms = memory_query["until_utc_ms"].asInt64();
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);
  if (since_ms < 0) since_ms = 0;

  std::string structured_path_filter =
    memory_query.isMember("structured_path") && memory_query["structured_path"].isString()
    ? trim_copy(memory_query["structured_path"].asString())
    : "";

  std::string key_prefix =
    memory_query.isMember("key_prefix") && memory_query["key_prefix"].isString()
    ? memory_query["key_prefix"].asString()
    : "";

  int limit = 50;
  if (memory_query.isMember("limit") && (memory_query["limit"].isInt() || memory_query["limit"].isUInt())) {
    limit = memory_query["limit"].asInt();
  }
  limit = std::max(1, std::min(1000, limit));

  const std::filesystem::path mem_root = memory_root_from_state_dir(state_dir);
  using namespace host_tools_internal;
  MemoryViewMode view;
  if (!memory_query["view"].isNull() && !memory_query["view"].isString()) return err_out("invalid memory view");
  if (!memory_view_mode((memory_query["view"].isNull() ? "current" : memory_query["view"].asString()), &view)) return err_out("invalid memory view");
  MemoryDocument source;Json::Value items, checkpoint;
  std::string source_path = structured_path_filter.empty() ? "STRUCTURED.md" : structured_path_filter;
  std::string pointer_prefix = "/items/", read_error;
  if (view == MemoryViewMode::Current) {
    if (memory_query.isMember("since_utc_ms") || memory_query.isMember("until_utc_ms")) return err_out("checkpoint time windows require historical/evidence view");
    if (memory_document_read(mem_root, source_path, &source, &read_error) != MemoryStoreStatus::Ok) return err_out(read_error);
    Json::Value doc;
    if (!memory_structured_parse_doc(source.text, &doc, &read_error)) return err_out(read_error);
    items = doc["items"];
  } else {
    std::vector<MemoryCheckpointMeta> rows;
    if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, 1, &rows, &read_error) || rows.empty()) return err_out("no checkpoints in window");
    const auto& meta = rows[0];source_path = meta.checkpoint_path_rel;pointer_prefix = "/doc/items/";
    if (memory_document_read(mem_root, source_path, &source, &read_error) != MemoryStoreStatus::Ok || !source.exists) return err_out("original checkpoint missing");
    if (source.revision != "sha256:" + meta.sha256) return err_out("checkpoint changed during query");
    Json::CharReaderBuilder reader;Json::Value doc;std::istringstream stream(source.text);
    if (!Json::parseFromStream(reader, stream, &doc, &read_error)) return err_out(read_error);
    items = doc["doc"]["items"];
    checkpoint["checkpoint_path"] = meta.checkpoint_path_rel;checkpoint["structured_path"] = meta.structured_path;
    checkpoint["sha256"] = meta.sha256;checkpoint["ts_utc"] = meta.ts_utc;checkpoint["ts_utc_ms"] = Json::Int64(meta.ts_utc_ms);checkpoint["bytes"] = Json::Int64(meta.bytes);
  }
  if (!items.isObject()) return err_out("memory source missing items");
  const auto as_of = memory_view_now_utc();
  std::vector<std::string> keys = items.getMemberNames();
  std::sort(keys.begin(), keys.end());

  Json::Value entries(Json::arrayValue);
  Json::Value entries_by_key(Json::objectValue);
  int returned = 0;
  for (const auto& key : keys) {
    if (returned >= limit) break;
    if (!key_prefix.empty() && key.rfind(key_prefix, 0) != 0) continue;
    const Json::Value rec = items[key];
    if (!rec.isObject() || (view == MemoryViewMode::Current && !memory_record_current(rec, as_of))) continue;
    Json::Value row(Json::objectValue);
    row["key"] = key;
    Json::Value visible = rec;if (view == MemoryViewMode::Current) visible.removeMember("versions");
    row["record"] = visible;
    MemoryPassage passage;passage.path = source_path;passage.source_id = source.source_id;passage.revision = source.revision;
    passage.pointer = pointer_prefix + memory_pointer_token(key) + "/value";
    row["evidence"] = memory_passage_evidence(passage);row["historical"] = view != MemoryViewMode::Current;
    entries.append(row);
    entries_by_key[key] = visible;
    returned++;
  }

  MemoryDocument current;
  if (memory_document_read(mem_root, source_path, &current, &read_error) != MemoryStoreStatus::Ok || current.revision != source.revision) return err_out("memory changed during query");
  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["assistant_text"] = "memory_query: ok";
  out["memory_root"] = mem_root.generic_string();
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  if (!structured_path_filter.empty()) out["structured_path_filter"] = structured_path_filter;
  if (!key_prefix.empty()) out["key_prefix"] = key_prefix;
  out["limit"] = limit;
  out["returned"] = returned;
  out["view"] = memory_view_name(view);out["as_of_utc"] = as_of;out["revision"] = source.revision;
  if (!checkpoint.isNull()) out["checkpoint"] = checkpoint;
  out["entries"] = entries;
  out["entries_by_key"] = entries_by_key;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  return out;
}

}  // namespace agentd
