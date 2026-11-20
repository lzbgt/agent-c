#pragma once
#if defined(AGENT_HAVE_JSONCPP)
#include "memory_store.h"
#include <chrono>
#include <json/json.h>
#include <set>
namespace host_tools_internal {
struct MemoryArchivePolicy {
  int max_age_days = 0; // zero disables expiry; age is archive filesystem mtime
  bool dry_run = true;
  size_t max_expirations = 128;
  std::set<std::string> protected_revisions; // host-owned receipts emitted by this pass
  std::function<bool()> cancelled;
  std::chrono::steady_clock::time_point deadline{};
  std::function<void(const std::string &)> before_expire;
  std::function<void(MemoryPublishPhase)> observer;
};
struct MemoryArchiveReport {
  bool complete = true, cancelled = false, budget_exhausted = false;
  size_t selected = 0, protected_count = 0, expired = 0, uncertain = 0, deferred = 0;
  uint64_t expired_bytes = 0;
  size_t errors_dropped = 0;
  Json::Value outcomes{Json::arrayValue}, errors{Json::arrayValue};
};
// Root-scoped history policy. Active public .md/.markdown/.json heads and literal
// sha256 references protect matching revisions conservatively, across source paths.
// External consumers must pin required revisions in an active memory document.
// No provider/model calls or effect dispatch occur here.
bool memory_archive_expire(const std::filesystem::path &root, const MemoryArchivePolicy &policy,
                           MemoryArchiveReport *report, std::string *error);
Json::Value memory_archive_report(const MemoryArchiveReport &report);
} // namespace host_tools_internal
#endif
