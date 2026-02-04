#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace host_tools_internal {

struct MemorySearchHit {
  std::string path;    // relative to memory root ("/"-separated)
  int line = 1;        // 1-based starting line of the chunk
  std::string snippet; // excerpt (may include lightweight markers)
  double score = 0.0;  // higher is better (implementation-defined)
};

// Best-effort retrieval:
// - Updates an on-disk index under `mem_root` when possible.
// - Performs ranked search restricted to `allowed_files_abs` (absolute paths).
//
// Returns true on success and fills `out_hits`. On failure returns false and sets `out_err`.
bool memory_index_search_ranked(
  const std::filesystem::path& mem_root,
  const std::vector<std::string>& allowed_files_abs,
  const std::string& query,
  int max_results,
  int max_snippet_chars,
  std::vector<MemorySearchHit>* out_hits,
  std::string* out_err
);

} // namespace host_tools_internal

