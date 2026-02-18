#pragma once

#include "memory_checkpoints.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agentd {

struct MemorySaliencePolicy {
  bool include_structured = true;
  bool include_daily = true;
  int daily_days = 7;
  int max_items = 12;
  int max_structured_items = 6;
  int max_daily_items = 6;
  double half_life_days = 14.0;
  double importance_weight = 0.35;
};

struct MemorySalienceItem {
  std::string tier;  // structured | daily
  std::string path;  // relative to memory root
  int line = 1;      // 1-based (daily only)
  std::string text;  // compact text snippet
  double score = 0.0;
  std::string ts_utc;
  int importance = -1;
  std::string key;   // structured key (structured only)
  std::string kind;  // structured kind
  std::string status;
};

struct MemorySalienceReport {
  int64_t generated_utc_ms = 0;
  bool structured_checkpoint_found = false;
  MemoryCheckpointMeta structured_checkpoint;
  std::vector<MemorySalienceItem> structured_items;
  std::vector<MemorySalienceItem> daily_items;
  std::vector<std::string> errors;
};

bool memory_salience_collect(
  const std::filesystem::path& memory_root,
  const MemorySaliencePolicy& policy,
  MemorySalienceReport* out_report,
  std::string* out_err
);

}  // namespace agentd
