#include "workflow_memory_correlate.h"

#include "json_util.h"
#include "memory_checkpoints.h"
#include "memory_correlation_graph.h"
#include "memory_correlation_index.h"
#include "memory_view.h"
#include "string_util.h"
#include <sstream>

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

  const auto reject = [&](const std::string &why) {
    if (out_err)
      *out_err = why;
    return err_out(why);
  };
  for (const auto *name : {"view", "structured_path", "key_prefix"})
    if (memory_correlate.isMember(name) && !memory_correlate[name].isString())
      return reject(std::string(name) + " must be a string");
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

  using namespace host_tools_internal;
  MemoryViewMode view;
  if ((!memory_correlate["view"].isNull() && !memory_correlate["view"].isString()) ||
      !memory_view_mode(memory_correlate.get("view", "current").asString(), &view))
    return reject("invalid correlation view");
  const std::filesystem::path mem_root = memory_root_from_state_dir(state_dir);
  if (view == MemoryViewMode::Current) {
    if (timeline || memory_correlate.isMember("since_utc_ms") ||
        memory_correlate.isMember("until_utc_ms"))
      return reject("correlation time windows require explicit historical/evidence view");
    const auto requested_path = memory_correlate.get("structured_path", "").asString();
    Json::Value rows, meta;
    std::string issue;
    if (!memory_correlation_index_query(
            mem_root, "trace:" + trace_id, max_entries, &rows, &meta, &issue,
            requested_path, memory_correlate.get("key_prefix", "").asString()))
      return reject(issue);
    Json::Value out;
    out["ok"] = true;
    out["trace_id"] = trace_id;
    out["needle"] = "trace:" + trace_id;
    out["view"] = "current";
    out["index"] = meta;
    out["incomplete"] = meta["incomplete"];
    out["since_utc_ms"] = Json::Int64(0);
    out["until_utc_ms"] = Json::Int64(INT64_MAX);
    out["entries"] = Json::Value(Json::arrayValue);
    out["daily_entries"] = Json::Value(Json::arrayValue);
    for (const auto &row : rows)
      out[row["kind"] == "structured" ? "entries" : "daily_entries"].append(row);
    out["tool_calls_total"] = Json::Int64(1);
    out["steps_executed"] = Json::Int64(1);
    out["assistant_text"] = "memory_correlate: current evidence";
    out["relationship_graph"] = memory_correlation_relationship_graph_from_response(trace_id, out);
    return out;
  }
  const std::filesystem::path ckdir = mem_root / "checkpoints";
  std::error_code ec;
  if (mem_root.empty() || !std::filesystem::exists(ckdir, ec) || !std::filesystem::is_directory(ckdir, ec)) {
    if (out_err) *out_err = "no checkpoints directory";
    return err_out("no checkpoints directory");
  }

  std::vector<MemoryCheckpointMeta> rows;
  std::string lerr;
  const int ck_limit = timeline ? 50 : 1;
  if (!memory_list_structured_checkpoints(
          mem_root, since_ms, until_ms,
          /*structured_path_filter=*/memory_correlate.get("structured_path", "").asString(),
          ck_limit, &rows, &lerr) ||
      rows.empty()) {
    if (out_err) *out_err = "no checkpoints in window";
    return err_out("no checkpoints in window");
  }

  const std::string needle = std::string("trace:") + trace_id;

  auto extract_entries = [&](const MemoryCheckpointMeta& ckmeta, Json::Value* out_entries, Json::Value* out_ckinfo) -> bool {
    if (!out_entries || !out_ckinfo) return false;
    *out_entries = Json::Value(Json::arrayValue);
    *out_ckinfo = Json::Value(Json::objectValue);

    if (ckmeta.checkpoint_path_rel.empty()) return false;
    Json::Value items(Json::nullValue);
    std::string rerr;
    MemoryDocument original;
    if (memory_document_read(mem_root, ckmeta.checkpoint_path_rel, &original, &rerr) !=
            MemoryStoreStatus::Ok ||
        original.revision != "sha256:" + ckmeta.sha256)
      return false;
    Json::CharReaderBuilder reader;
    Json::Value doc;
    std::istringstream input(original.text);
    if (!Json::parseFromStream(reader, input, &doc, &rerr))
      return false;
    items = doc["doc"]["items"];
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
      const auto prefix = memory_correlate.get("key_prefix", "").asString();
      if (!prefix.empty() && key.rfind(prefix, 0) != 0)
        continue;
      const Json::Value rec = items[key];
      if (!rec.isObject()) continue;
      const Json::Value sources = rec.isMember("sources") ? rec["sources"] : Json::Value(Json::nullValue);
      if (!sources.isArray()) continue;
      bool hit = false;
      for (Json::ArrayIndex i = 0; i < sources.size(); i++) {
        if (!sources[i].isString()) continue;
        const std::string s = sources[i].asString();
        if (memory_correlation_source_matches(s, needle)) {
          hit = true;
          break;
        }
      }
      if (!hit) continue;
      if(added>=max_entries){(*out_ckinfo)["incomplete"]=true;continue;}
      Json::Value row(Json::objectValue);
      row["key"] = key;
      row["record"] = rec;
      row["historical"] = true;
      MemoryPassage evidence;
      evidence.path = ckmeta.checkpoint_path_rel;
      evidence.source_id = original.source_id;
      evidence.revision = original.revision;
      evidence.pointer = "/doc/items/" + memory_pointer_token(key) + "/value";
      row["evidence"] = memory_passage_evidence(evidence);
      (*out_entries).append(row);
      added++;
    }
    MemoryDocument final;
    return memory_document_read(mem_root, ckmeta.checkpoint_path_rel, &final, &rerr) ==
               MemoryStoreStatus::Ok &&
           final.revision == original.revision;
  };

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["view"] = memory_view_name(view);
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
    out["relationship_graph"] = memory_correlation_relationship_graph_from_response(trace_id, out);
    out["assistant_text"] = "memory_correlate entries=" + std::to_string((unsigned)entries.size());
    return out;
  }

  Json::Value timeline_arr(Json::arrayValue);
  for (size_t i = 0; i < rows.size(); i++) {
    Json::Value entries, ckinfo;
    if (!extract_entries(rows[i], &entries, &ckinfo))
      return reject("checkpoint changed or unavailable during historical correlation");
    Json::Value row(Json::objectValue);
    row["checkpoint"] = ckinfo;
    row["entries"] = entries;
    timeline_arr.append(row);
    if ((int)timeline_arr.size() >= 50) break;
  }
  out["timeline"] = timeline_arr;
  out["relationship_graph"] = memory_correlation_relationship_graph_from_response(trace_id, out);
  out["assistant_text"] = "memory_correlate timeline=" + std::to_string((unsigned)timeline_arr.size());
  return out;
}

}  // namespace agentd
