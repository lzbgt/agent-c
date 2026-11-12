#pragma once
#include <json/json.h>
#include <filesystem>
#include <string>

// A host-side, transactionally persisted requirement/blocker ledger. Each mutation
// changes one item; open blockers affect only their explicit requirement IDs.
bool work_ledger_apply(const std::filesystem::path& path, const Json::Value& request,
                       Json::Value* response, std::string* error);

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

// Process-local wake hints only. Callers must reread the authoritative ledger.
// Subscribing does not create a file or mutate requirements. Other processes
// are discovered by a bounded read fallback at the consumer.
class WorkLedgerWatch {
 public:
  uint64_t generation();
  bool wait_for(uint64_t observed, std::chrono::milliseconds timeout);
  void notify();
 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t generation_ = 0;
};
std::shared_ptr<WorkLedgerWatch> work_ledger_subscribe(const std::filesystem::path& path);
