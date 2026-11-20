#pragma once

#include <filesystem>
#include <string>

#include <json/json.h>

namespace agentd {

struct MemoryCorrelationIndexOptions {
  int daily_days = 14;
  bool include_structured = true;
  bool include_daily = true;
};

struct MemoryCorrelationIndexReport {
  int64_t generated_utc_ms = 0;
  std::string generated_utc;
  int token_count = 0;
  int entry_count = 0;
  int structured_entries = 0;
  int daily_entries = 0;
};

bool memory_correlation_source_matches(const std::string &source, const std::string &token);
MemoryCorrelationIndexOptions memory_correlation_index_default_options();

bool memory_correlation_index_build(const std::filesystem::path &memory_root,
                                    const MemoryCorrelationIndexOptions &opt,
                                    MemoryCorrelationIndexReport *out_report, std::string *out_err);

bool memory_correlation_index_query(const std::filesystem::path &memory_root,
                                    const std::string &token, int max_entries,
                                    Json::Value *out_entries, Json::Value *out_meta,
                                    std::string *out_err, const std::string &structured_path = "",
                                    const std::string &key_prefix = "");

} // namespace agentd
