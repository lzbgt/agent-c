#pragma once

#include <filesystem>
#include <string>

#include <json/json.h>

namespace agentd {

struct MemoryCorrelationIndexOptions {
  int daily_days = 14;
  int max_daily_entries = 200;
  int max_structured_checkpoints = 10;
  int max_structured_entries = 500;
  int max_entries_per_token = 100;
  int max_recaps = 200;
  size_t value_excerpt_chars = 160;
  bool include_structured = true;
  bool include_daily = true;
  bool include_recaps = true;
};

struct MemoryCorrelationIndexReport {
  int64_t generated_utc_ms = 0;
  std::string generated_utc;
  std::string index_path;
  int token_count = 0;
  int entry_count = 0;
  int structured_entries = 0;
  int daily_entries = 0;
  int recap_entries = 0;
};

MemoryCorrelationIndexOptions memory_correlation_index_default_options();

bool memory_correlation_index_build(
  const std::filesystem::path& memory_root,
  const MemoryCorrelationIndexOptions& opt,
  MemoryCorrelationIndexReport* out_report,
  std::string* out_err
);

bool memory_correlation_index_query(
  const std::filesystem::path& memory_root,
  const std::string& token,
  int max_entries,
  Json::Value* out_entries,
  Json::Value* out_meta,
  std::string* out_err
);

}  // namespace agentd
