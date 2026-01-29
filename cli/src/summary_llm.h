#pragma once

#include "openai_client.h"
#include "summary_compaction.h"

#include <cstddef>
#include <string>

struct CompactionSummaryResult {
  bool ok = false;
  std::string summary_text;
  std::string error;
  long http_status = 0;
  std::string http_body;
};

// Calls an OpenAI-compatible backend to produce a short summary of the dropped region.
//
// Notes:
// - This is host-only (CLI/daemon). Core stays provider-agnostic.
// - The summary is intended to be inserted as a system message during compaction.
CompactionSummaryResult generate_compaction_summary_via_llm(
  const OpenAIClientConfig& base_cfg,
  const std::string& summary_model,
  const SummaryCompactionInput& input,
  size_t max_summary_chars = 1200
);

