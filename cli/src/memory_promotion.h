#pragma once
#include "memory_view.h"
#include "toolset_host.h"
#include <functional>
#include <map>

namespace host_tools_internal {
constexpr size_t kMemoryPromotionReceiptLimit = 4096;
struct MemoryMarker {
  std::string id, key, kind, status, value, base;
  MemoryPassage source;
};
struct MemoryMarkerScan {
  std::vector<MemoryMarker> markers;
  Json::Value errors{Json::arrayValue};
};
// False + empty error means a non-marker line. Recognized malformed markers
// return a reason instead of silently becoming new facts.
bool memory_marker_fields(const std::string &line, MemoryMarker *marker,
                          std::string *error);
bool memory_marker_scan(const std::string &path, const MemoryDocument &document,
                        MemoryMarkerScan *scan, std::string *error);
std::string memory_marker_id(const MemoryMarker &marker);
struct MemoryPromotionPlan {
  MemoryViewSnapshot snapshot;
  std::vector<MemoryMarker> markers;
  Json::Value errors{Json::arrayValue};
  size_t max_keys = 256;
  int keep_checkpoints = 100;
  std::function<void(MemoryPublishPhase)> publish_observer;
};
struct MemoryPromotionUpdate {
  Json::Value entries{Json::arrayValue}, ledger, report;
  std::map<std::string, std::vector<MemoryMarker>> accepted;
  size_t new_receipts = 0, deferred = 0;
};
bool memory_promotion_capture(const std::filesystem::path &root,
                              const std::vector<std::string> &files,
                              size_t max_file_bytes, size_t max_keys,
                              MemoryPromotionPlan *plan, std::string *error);
bool memory_promotion_validate_dependencies(MemoryViewSnapshot *snapshot,
                                            std::string *error);
bool memory_promotion_ledger_valid(const Json::Value &document,
                                   std::string *error);
// Called only inside the ordinary memory_put root writer transaction.
bool memory_promotion_prepare(const std::filesystem::path &root,
                              const Json::Value &document,
                              const MemoryPromotionPlan &plan,
                              MemoryPromotionUpdate *update, std::string *error,
                              bool retain_sources = true);
bool memory_promotion_finish(Json::Value *document,
                             const MemoryPromotionPlan &plan,
                             MemoryPromotionUpdate *update,
                             bool *metadata_changed, std::string *error);
// Native host operation, not a model-exposed tool or JSON configuration switch.
agent_status_t memory_promote(const HostToolsetConfig &config,
                              const MemoryPromotionPlan &plan,
                              Json::Value *receipt, std::string *error);
} // namespace host_tools_internal
