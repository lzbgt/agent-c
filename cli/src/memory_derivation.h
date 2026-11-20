#pragma once
#include "memory_view.h"
namespace host_tools_internal {
// A current input set is provenance, never semantic verification of generated
// claims.
Json::Value memory_derivation_make(const MemoryViewSnapshot &snapshot, const Json::Value &evidence);
bool memory_derivation_paths(const Json::Value &manifest, std::vector<std::string> *paths,
                             std::string *error);
std::vector<Json::Value> memory_derivation_evaluate_many(const std::vector<Json::Value> &manifests,
                                                         const MemoryViewSnapshot &current);
Json::Value memory_derivation_evaluate(const Json::Value &manifest,
                                       const MemoryViewSnapshot &current);
bool memory_derivation_capture(const std::filesystem::path &root,
                               const std::vector<Json::Value> &manifests,
                               MemoryViewSnapshot *current, std::string *error);
} // namespace host_tools_internal
