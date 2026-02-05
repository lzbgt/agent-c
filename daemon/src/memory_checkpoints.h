#pragma once

#include <json/json.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agentd {

struct MemoryCheckpointMeta {
  int64_t ts_utc_ms = 0;
  std::string ts_utc;
  std::string checkpoint_path_rel; // relative to memory root (e.g. checkpoints/structured_....json)
  std::string structured_path;     // checkpoint "path" field (e.g. STRUCTURED.md)
  std::string sha256;              // sha256 of checkpoint JSON bytes (lowercase hex)
  int64_t bytes = 0;
};

// Scan `${memory_root}/checkpoints/structured_*.json` and return checkpoint metadata sorted newest-first.
// - time window is inclusive (since_utc_ms..until_utc_ms)
// - if structured_path_filter is non-empty, only include checkpoints whose internal `path` matches
// - limit is clamped to [1..200]
bool memory_list_structured_checkpoints(
  const std::filesystem::path& memory_root,
  int64_t since_utc_ms,
  int64_t until_utc_ms,
  const std::string& structured_path_filter,
  int limit,
  std::vector<MemoryCheckpointMeta>* out,
  std::string* out_err
);

// Read and parse a single structured checkpoint JSON file (relative to memory_root).
// Returns:
// - structured_path_out: checkpoint "path" value (default STRUCTURED.md)
// - items_out: checkpoint doc.items object
bool memory_read_structured_checkpoint_items(
  const std::filesystem::path& memory_root,
  const std::string& checkpoint_path_rel,
  std::string* structured_path_out,
  Json::Value* items_out,
  std::string* out_err
);

}  // namespace agentd

