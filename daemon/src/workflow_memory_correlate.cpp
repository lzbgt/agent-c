#include "workflow_memory_correlate.h"

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

Json::Value workflow_memory_correlate_to_json(
  const std::string& state_dir,
  const std::string& workflow_trace_id,
  const Json::Value& memory_correlate,
  std::string* out_err
) {
  if (out_err) out_err->clear();

  if (!memory_correlate.isObject()) {
    if (out_err) *out_err = "memory_correlate must be an object";
    return err_out("memory_correlate must be an object");
  }

  std::string trace_id =
    memory_correlate.isMember("trace_id") && memory_correlate["trace_id"].isString()
    ? trim_copy(memory_correlate["trace_id"].asString())
    : "";
  if (trace_id.empty()) trace_id = workflow_trace_id;
  if (trace_id.empty()) {
    if (out_err) *out_err = "memory_correlate.trace_id is required (or workflow trace_id must be set)";
    return err_out("memory_correlate.trace_id is required (or workflow trace_id must be set)");
  }

  int64_t since_ms = 0;
  int64_t until_ms = INT64_MAX;
  if (memory_correlate.isMember("since_utc_ms") && (memory_correlate["since_utc_ms"].isInt64() || memory_correlate["since_utc_ms"].isUInt64() || memory_correlate["since_utc_ms"].isInt() || memory_correlate["since_utc_ms"].isUInt())) {
    since_ms = memory_correlate["since_utc_ms"].asInt64();
  }
  if (memory_correlate.isMember("until_utc_ms") && (memory_correlate["until_utc_ms"].isInt64() || memory_correlate["until_utc_ms"].isUInt64() || memory_correlate["until_utc_ms"].isInt() || memory_correlate["until_utc_ms"].isUInt())) {
    until_ms = memory_correlate["until_utc_ms"].asInt64();
  }
  if (until_ms < since_ms) std::swap(until_ms, since_ms);
  if (since_ms < 0) since_ms = 0;

  int max_entries = 50;
  if (memory_correlate.isMember("max_entries") && (memory_correlate["max_entries"].isInt() || memory_correlate["max_entries"].isUInt())) {
    max_entries = memory_correlate["max_entries"].asInt();
  }
  max_entries = std::max(1, std::min(500, max_entries));

  const bool timeline =
    memory_correlate.isMember("timeline") && memory_correlate["timeline"].isBool() ? memory_correlate["timeline"].asBool() : false;

  const std::filesystem::path mem_root = memory_root_from_state_dir(state_dir);
  const std::filesystem::path ckdir = mem_root / "checkpoints";
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(ckdir, ec) || !std::filesystem::is_directory(ckdir, ec)) {
    if (out_err) *out_err = "no checkpoints directory";
    return err_out("no checkpoints directory");
  }

  std::vector<MemoryCheckpointMeta> rows;
  std::string lerr;
  const int ck_limit = timeline ? 50 : 1;
  if (!memory_list_structured_checkpoints(mem_root, since_ms, until_ms, /*structured_path_filter=*/"", ck_limit, &rows, &lerr) || rows.empty()) {
    if (out_err) *out_err = "no checkpoints in window";
    return err_out("no checkpoints in window");
  }

  const std::string needle = std::string("trace:") + trace_id;

  auto extract_entries = [&](const MemoryCheckpointMeta& ckmeta, Json::Value* out_entries, Json::Value* out_ckinfo) -> bool {
    if (!out_entries || !out_ckinfo) return false;
    *out_entries = Json::Value(Json::arrayValue);
    *out_ckinfo = Json::Value(Json::objectValue);

    if (ckmeta.checkpoint_path_rel.empty()) return false;
    std::string structured_path2;
    Json::Value items(Json::nullValue);
    std::string rerr;
    if (!memory_read_structured_checkpoint_items(mem_root, ckmeta.checkpoint_path_rel, &structured_path2, &items, &rerr)) return false;
    if (!items.isObject()) return false;

    (*out_ckinfo)["checkpoint_path"] = ckmeta.checkpoint_path_rel;
    (*out_ckinfo)["structured_path"] = ckmeta.structured_path;
    (*out_ckinfo)["sha256"] = ckmeta.sha256;
    (*out_ckinfo)["ts_utc"] = ckmeta.ts_utc;
    (*out_ckinfo)["ts_utc_ms"] = (Json::Int64)ckmeta.ts_utc_ms;
    (*out_ckinfo)["bytes"] = (Json::Int64)ckmeta.bytes;

    std::vector<std::string> keys = items.getMemberNames();
    std::sort(keys.begin(), keys.end());
    int added = 0;
    for (const auto& key : keys) {
      if (added >= max_entries) break;
      const Json::Value rec = items[key];
      if (!rec.isObject()) continue;
      const Json::Value sources = rec.isMember("sources") ? rec["sources"] : Json::Value(Json::nullValue);
      if (!sources.isArray()) continue;
      bool hit = false;
      for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
        if (!sources[i].isString()) continue;
        const std::string s = sources[i].asString();
        if (s.find(needle) != std::string::npos) { hit = true; break; }
      }
      if (!hit) continue;
      Json::Value row(Json::objectValue);
      row["key"] = key;
      row["record"] = rec;
      (*out_entries).append(row);
      added++;
    }
    return true;
  };

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["trace_id"] = trace_id;
  out["needle"] = needle;
  out["since_utc_ms"] = (Json::Int64)since_ms;
  out["until_utc_ms"] = (Json::Int64)until_ms;
  out["tool_calls_total"] = (Json::Int64)1;
  out["steps_executed"] = (Json::Int64)1;

  if (!timeline) {
    Json::Value entries, ckinfo;
    if (!extract_entries(rows[0], &entries, &ckinfo)) {
      if (out_err) *out_err = "failed to extract entries from newest checkpoint";
      return err_out("failed to extract entries from newest checkpoint");
    }
    out["checkpoint"] = ckinfo;
    out["entries"] = entries;
    out["assistant_text"] = "memory_correlate entries=" + std::to_string((unsigned)entries.size());
    return out;
  }

  Json::Value timeline_arr(Json::arrayValue);
  for (size_t i = 0; i < rows.size(); i++) {
    Json::Value entries, ckinfo;
    if (!extract_entries(rows[i], &entries, &ckinfo)) continue;
    Json::Value row(Json::objectValue);
    row["checkpoint"] = ckinfo;
    row["entries"] = entries;
    timeline_arr.append(row);
    if ((int)timeline_arr.size() >= 50) break;
  }
  out["timeline"] = timeline_arr;
  out["assistant_text"] = "memory_correlate timeline=" + std::to_string((unsigned)timeline_arr.size());
  return out;
}

}  // namespace agentd
