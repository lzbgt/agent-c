#include "workflow_memory_query.h"

#include "json_util.h"
#include "memory_checkpoints.h"
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
  std::vector<MemoryCheckpointMeta> rows;
  std::string lerr;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, structured_path_filter, /*limit=*/1, &rows, &lerr) || rows.empty()) {
    if (out_err) *out_err = "no checkpoints in window";
    return err_out("no checkpoints in window");
  }
  const MemoryCheckpointMeta& ckmeta = rows[0];

  std::string structured_path2;
  Json::Value items(Json::nullValue);
  std::string rerr;
  if (!memory_read_structured_checkpoint_items(mem_root, ckmeta.checkpoint_path_rel, &structured_path2, &items, &rerr)) {
    if (out_err) *out_err = "failed to read checkpoint";
    return err_out("failed to read checkpoint");
  }
  if (!items.isObject()) {
    if (out_err) *out_err = "checkpoint missing doc.items";
    return err_out("checkpoint missing doc.items");
  }

  std::vector<std::string> keys = items.getMemberNames();
  std::sort(keys.begin(), keys.end());

  Json::Value entries(Json::arrayValue);
  Json::Value entries_by_key(Json::objectValue);
  int returned = 0;
  for (const auto& key : keys) {
    if (returned >= limit) break;
    if (!key_prefix.empty() && key.rfind(key_prefix, 0) != 0) continue;
    const Json::Value rec = items[key];
    if (!rec.isObject()) continue;
    Json::Value row(Json::objectValue);
    row["key"] = key;
    row["record"] = rec;
    entries.append(row);
    entries_by_key[key] = rec;
    returned++;
  }

  Json::Value ckinfo(Json::objectValue);
  ckinfo["checkpoint_path"] = ckmeta.checkpoint_path_rel;
  ckinfo["structured_path"] = ckmeta.structured_path;
  ckinfo["sha256"] = ckmeta.sha256;
  ckinfo["ts_utc"] = ckmeta.ts_utc;
  ckinfo["ts_utc_ms"] = (Json::Int64)ckmeta.ts_utc_ms;
  ckinfo["bytes"] = (Json::Int64)ckmeta.bytes;

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
  out["checkpoint"] = ckinfo;
  out["entries"] = entries;
  out["entries_by_key"] = entries_by_key;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;
  return out;
}

}  // namespace agentd
