#pragma once
#include "memory_index.h"
#if defined(AGENT_HAVE_JSONCPP)
#include <chrono>
#include <functional>

namespace host_tools_internal {
struct MemoryScoringPassage {
  std::string id, text;
};
struct MemoryScoringRequest {
  std::string question;
  std::vector<MemoryScoringPassage> accumulated, candidates;
  // Includes the caller's cancellation and the retrieval's total deadline.
  std::function<bool()> cancelled;
};
struct MemoryScoringResult {
  std::vector<std::pair<std::string, double>> scores;
  std::string provider, model, prompt_revision;
  Json::Value usage;
};
using MemoryScorer =
    std::function<bool(const MemoryScoringRequest &, MemoryScoringResult *, std::string *)>;
enum class MemoryRetrievalStrategy { Lexical, Rerank, Expand };
struct MemoryRetrievalOptions {
  MemoryRetrievalStrategy strategy = MemoryRetrievalStrategy::Lexical;
  size_t initial_candidates = 20, new_candidates = 20, retained = 5, final_records = 8;
  size_t expansion_terms = 8, snippet_bytes = 600, context_bytes = 16000;
  size_t scoring_input_bytes = 24000;
  double seed_threshold = 0.5;
  std::chrono::milliseconds timeout{20000};
  std::function<bool()> cancelled;
};
struct MemoryRetrievalResult {
  std::vector<MemorySearchHit> hits;
  // Exact compact-JSON serialization is bounded by options.context_bytes.
  Json::Value context{Json::arrayValue}, report;
};
// One captured eligibility/revision set for all lexical, scoring and expansion work.
// Cancellation or revision changes clear output. Scorer failures preserve an
// explicitly incomplete lexical fallback; caller must inspect report.complete.
bool memory_retrieve_evidence(const MemoryViewSnapshot &, const std::string &question,
                              const MemoryRetrievalOptions &, const MemoryScorer &,
                              MemoryRetrievalResult *, std::string *error);
Json::Value memory_scoring_payload(const MemoryScoringRequest &);
std::string memory_retrieval_strategy_name(MemoryRetrievalStrategy);
} // namespace host_tools_internal
#endif
