#pragma once
#include "memory_checkpoints.h"
#include "memory_view.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace agentd {
struct MemorySaliencePolicy {
  bool include_structured = true, include_daily = true;
  int daily_days = 7, max_items = 12, max_structured_items = 6, max_daily_items = 6;
  double half_life_days = 14.0, importance_weight = 0.35;
  host_tools_internal::MemoryViewMode view = host_tools_internal::MemoryViewMode::Current;
};
struct MemorySalienceItem {
  std::string tier, path;
  int line = 1;
  std::string text;
  double score = 0.0;
  std::string ts_utc;
  int importance = -1;
  std::string key, kind, status;
  bool historical = false;
  Json::Value evidence;
};
struct MemorySalienceReport {
  int64_t generated_utc_ms = 0;
  bool structured_checkpoint_found = false; // historical checkpoint is never the current authority
  MemoryCheckpointMeta structured_checkpoint;
  std::vector<MemorySalienceItem> structured_items, daily_items;
  std::vector<std::string> errors;
  host_tools_internal::MemoryViewSnapshot snapshot;
};
void memory_salience_rank(const host_tools_internal::MemoryViewSnapshot &snapshot,
                          const MemorySaliencePolicy &policy, MemorySalienceReport *report);
bool memory_salience_collect(const std::filesystem::path &root, const MemorySaliencePolicy &policy,
                             MemorySalienceReport *report, std::string *error);
} // namespace agentd
