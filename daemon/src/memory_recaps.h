#pragma once

#include "daemon_config.h"
#include "memory_salience.h"
#include "openai_client.h"

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace agentd {

struct MemoryRecapOptions {
  MemorySaliencePolicy salience;
  std::string model;        // optional override; falls back to summary_model when empty
  size_t summary_max_chars = 1200;
  bool dry_run = false;
  bool write_file = true;
};

struct MemoryRecapReport {
  int64_t generated_utc_ms = 0;
  std::string ts_utc;
  std::string model;
  bool dry_run = false;
  bool write_file = true;
  MemorySaliencePolicy policy;
  MemorySalienceReport salience;
  std::string summary_text;
  Json::Value summary_json;
  bool summary_json_ok = false;
  std::string recap_path_rel;
  int64_t recap_bytes = 0;
  std::string prompt;
  bool prompt_truncated = false;
};

bool memory_generate_recap(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const MemoryRecapOptions& opt,
  MemoryRecapReport* out_report,
  std::string* out_err
);

bool memory_list_recaps(
  const DaemonConfig& cfg,
  int limit,
  bool include_summary,
  Json::Value* out_list,
  std::string* out_err
);

Json::Value memory_recap_report_to_json(const MemoryRecapReport& rep, bool include_prompt);

}  // namespace agentd
