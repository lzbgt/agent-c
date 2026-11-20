#pragma once
#include <filesystem>
#include <string>
#include <vector>
#if defined(AGENT_HAVE_JSONCPP)
#include "memory_view.h"
#endif
namespace host_tools_internal {
struct MemorySearchHit {
  std::string path;
  int line = 1;
  std::string snippet;
  double score = 0.0;
#if defined(AGENT_HAVE_JSONCPP)
  MemoryPassage passage;
#endif
};
bool memory_index_search_ranked(const std::filesystem::path &root, const std::vector<std::string> &files,
                                const std::string &query, int max_results, int max_snippet_chars,
                                std::vector<MemorySearchHit> *hits, std::string *error);
#if defined(AGENT_HAVE_JSONCPP)
bool memory_retrieve_snapshot(const MemoryViewSnapshot &snapshot, const std::string &query, bool use_index,
                              bool case_sensitive, int max_results, int max_snippet_chars,
                              std::vector<MemorySearchHit> *hits, std::string *engine, std::string *error,
                              std::string *index_error = nullptr, int context_lines = 2);
// FTS and substring fallback share exactly the same captured eligibility projection.
// A concurrent change clears results and fails; it never falls back to stale raw text.
bool memory_retrieve(const std::filesystem::path &root, const std::vector<std::string> &files,
                     const std::string &query, MemoryViewMode mode, bool use_index, bool case_sensitive,
                     int max_results, int max_snippet_chars, std::vector<MemorySearchHit> *hits,
                     std::string *engine, std::string *error, std::string *index_error = nullptr,
                     int context_lines = 2);
#endif
} // namespace host_tools_internal
