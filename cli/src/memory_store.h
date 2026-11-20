#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace host_tools_internal {
constexpr size_t kMemoryDocumentMaxBytes = 8 * 1024 * 1024;
enum class MemoryStoreStatus { Ok, Invalid, Unavailable, Conflict, Cancelled, Busy, Uncertain };
enum class MemoryPublishPhase { Archived, BeforeReplace, AfterReplace };
struct MemoryDocument {
  bool exists = false;
  std::string text;
  std::string revision = "absent"; // SHA-256 of exact bytes; absent differs from an empty file.
  std::string source_id;
};

bool memory_revision_valid(const std::string& revision);
// Retain a verified actual source head while holding the memory-root writer lock.
bool memory_document_retain(const std::filesystem::path& root, const std::string& path,
  const MemoryDocument& document, std::string* error);
// Immutable auxiliary receipt. Caller holds the memory root's writer transaction.
bool memory_document_publish_immutable(const std::filesystem::path& root, const std::string& relative,
  const std::string& text, std::string* error);
MemoryStoreStatus memory_document_read(const std::filesystem::path& root, const std::string& relative_path,
                                       MemoryDocument* out, std::string* error);
MemoryStoreStatus memory_document_read_revision(const std::filesystem::path& root, const std::string& relative_path,
    const std::string& revision, MemoryDocument* out, std::string* error);

// Disposable derived artifacts share a cross-process lease with source writers.
// Lock order: source writer -> cache; readers take only cache, never writer.
class MemoryCacheLease {
public:
  MemoryCacheLease();
  ~MemoryCacheLease();
  MemoryCacheLease(const MemoryCacheLease &) = delete;
  MemoryCacheLease &operator=(const MemoryCacheLease &) = delete;
  MemoryStoreStatus begin(const std::filesystem::path &root, std::function<bool()> cancelled,
                          std::string *error);
  bool purge(std::string *error);
  bool discard_obsolete(std::string *error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// All memory API writers share a memory-root OS lock. A replacement supplies
// the revision it read; append operations may capture the current revision while
// holding the lock. A commit retains original bytes before atomic publication.
class MemoryDocumentTransaction {
 public:
  MemoryDocumentTransaction();
  ~MemoryDocumentTransaction();
  MemoryDocumentTransaction(const MemoryDocumentTransaction&) = delete;
  MemoryDocumentTransaction& operator=(const MemoryDocumentTransaction&) = delete;
  MemoryStoreStatus begin(const std::filesystem::path& root, const std::string& relative_path,
    const std::optional<std::string>& expected_revision, std::function<bool()> cancelled, std::string* error);
  MemoryStoreStatus commit(const std::string& text, std::string* error, size_t keep_revisions = 100,
    const std::function<void(MemoryPublishPhase)>& observer = {});
  // Retain the original, atomically unlink the active head and sync its directory.
  // Requires begin() with an explicit expected revision; no hard forgetting.
  MemoryStoreStatus retire(std::string* error,
    const std::function<void(MemoryPublishPhase)>& observer = {});
  // Retire a different source using this transaction's already-held root lock.
  // Auxiliary maintenance never changes the primary publication receipt.
  MemoryStoreStatus retire_auxiliary(const std::string& relative_path,
    const std::string& expected_revision, std::string* error);
  const MemoryDocument& document() const;
  bool published() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace host_tools_internal
