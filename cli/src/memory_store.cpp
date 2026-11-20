#include "memory_store.h"
#include "agent_sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <fstream>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace host_tools_internal {
namespace {
namespace fs = std::filesystem;
using Status = MemoryStoreStatus;
std::string digest(const std::string& text) {
  char hex[65]{};agent_sha256_hex_of_bytes(text.data(), text.size(), hex);return std::string("sha256:") + hex;
}
Status fail(Status status, const char* message, std::string* error) {
  if (error) *error = message;
  return status;
}
bool regular(const fs::path& path, bool* exists) {
  std::error_code ec;const auto state = fs::symlink_status(path, ec);
  *exists = state.type() != fs::file_type::not_found && ec != std::errc::no_such_file_or_directory;
  return (!ec || ec == std::errc::no_such_file_or_directory) && (!*exists || fs::is_regular_file(state));
}
bool resolve(const fs::path& input, const std::string& relative, fs::path* root, fs::path* path) {
  if (input.empty() || relative.empty() || relative.size() > 1024 || relative.find('\0') != std::string::npos ||
      relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos) return false;
  const fs::path name(relative);
  if (name.is_absolute() || name.has_root_name()) return false;
  for (const auto& part : name) if (part == "." || part == "..") return false;
  std::error_code ec;const auto absolute = fs::absolute(input, ec);
  if (ec) return false;
  *root = fs::weakly_canonical(absolute, ec);
  if (ec) return false;
  *path = *root / name;
  fs::path current = *root;
  for (const auto& part : name) {
    current /= part;
    const auto state = fs::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory) { ec.clear();continue; }
    if (ec || fs::is_symlink(state)) return false;
  }
  return true;
}
Status read(const fs::path& path, const std::string& source, MemoryDocument* out, std::string* error) {
  if (!out) return fail(Status::Invalid, "missing memory snapshot output", error);
  *out = {};out->source_id = source;
  bool exists = false;
  if (!regular(path, &exists)) return fail(Status::Invalid, "memory source must be a regular file, not a link", error);
  if (!exists) return Status::Ok;
  std::error_code ec;const auto size = fs::file_size(path, ec);
  if (ec || size > kMemoryDocumentMaxBytes) return fail(Status::Unavailable, "memory document unavailable or exceeds 8 MiB", error);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail(Status::Unavailable, "cannot read memory document", error);
  // Read from one open inode. An atomic replacement cannot mix its old/new bytes.
  std::string text;char buffer[16384];
  while (stream) {
    stream.read(buffer, sizeof(buffer));text.append(buffer, size_t(stream.gcount()));
    if (text.size() > kMemoryDocumentMaxBytes) return fail(Status::Unavailable, "memory document exceeds 8 MiB", error);
  }
  if (!stream.eof()) return fail(Status::Unavailable, "memory document read failed", error);
  out->exists = true;out->text = std::move(text);out->revision = digest(out->text);return Status::Ok;
}
std::string source_id(const fs::path& root, const std::string& name) {
  return "memory:" + digest(root.generic_string() + '\n' + name).substr(7);
}
fs::path archive(const fs::path& root, const std::string& name, const std::string& revision) {
  return root / ".revisions" / digest(name).substr(7) / (revision.substr(7) + ".blob");
}
bool sync_directory(const fs::path& path) {
#if defined(_WIN32)
  // MoveFileExW uses WRITE_THROUGH below. Directory FlushFileBuffers is not a
  // portable Windows operation; runtime durability needs platform qualification.
  (void)path;return true;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;const bool closed = ::close(fd) == 0;return ok && closed;
#endif
}
bool directory(const fs::path& path) {
  std::error_code ec;const auto state = fs::symlink_status(path, ec);
  if (!ec && fs::exists(state)) return fs::is_directory(state) && !fs::is_symlink(state);
  if (ec && ec != std::errc::no_such_file_or_directory) return false;
  if (path.empty() || path == path.parent_path() || !directory(path.parent_path())) return false;
  const bool created = fs::create_directory(path, ec);
  if (ec) return false;
#if !defined(_WIN32)
  if (created && ::chmod(path.c_str(), 0700) != 0) return false;
#endif
  return !created || sync_directory(path.parent_path());
}
class FileLock {
 public:
  ~FileLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  Status acquire(const fs::path& path, const std::function<bool()>& cancelled, std::string* error) {
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};
    if (handle_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle_, &information) ||
        information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      return fail(Status::Unavailable, "cannot open memory lock", error);
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat state{};
    if (fd_ < 0 || ::fstat(fd_, &state) != 0 || !S_ISREG(state.st_mode))
      return fail(Status::Unavailable, "cannot open memory lock", error);
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      if (cancelled && cancelled()) return fail(Status::Cancelled, "memory update cancelled", error);
#if defined(_WIN32)
      OVERLAPPED overlap{};
      if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlap)) return Status::Ok;
      if (GetLastError() != ERROR_LOCK_VIOLATION) return fail(Status::Unavailable, "memory lock failed", error);
#else
      if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) return Status::Ok;
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) return fail(Status::Unavailable, "memory lock failed", error);
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return fail(Status::Busy, "memory document busy; reread before retrying", error);
  }
 private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};
class StagedFile {
 public:
  ~StagedFile() { if (!temporary_.empty()) { std::error_code ec;fs::remove(temporary_, ec); } }
  bool prepare(const fs::path& destination, const std::string& text) {
    if (!directory(destination.parent_path())) return false;
    static std::atomic<uint64_t> serial{0};
    for (int attempt = 0; attempt < 32; ++attempt) {
      temporary_ = destination;temporary_ += ".memory-tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
#if defined(_WIN32)
      const auto handle = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (handle == INVALID_HANDLE_VALUE) { temporary_.clear();if (GetLastError() == ERROR_FILE_EXISTS) continue;return false; }
      DWORD written = 0;const bool ok = WriteFile(handle, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(handle);
      const bool closed = CloseHandle(handle) != 0;return ok && closed;
#else
      const int fd = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0) { temporary_.clear();if (errno == EEXIST) continue;return false; }
      size_t offset = 0;
      while (offset < text.size()) {
        const auto amount = ::write(fd, text.data() + offset, text.size() - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) break;
        offset += size_t(amount);
      }
      const bool ok = offset == text.size() && ::fsync(fd) == 0;
      const bool closed = ::close(fd) == 0;return ok && closed;
#endif
    }
    return false;
  }
  bool publish(const fs::path& path, bool replace) {
#if defined(_WIN32)
    if (!MoveFileExW(temporary_.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) return false;
#else
    if (replace) { if (::rename(temporary_.c_str(), path.c_str()) != 0) return false; }
    else {
      // Link claims the immutable object without overwriting an existing name.
      if (::link(temporary_.c_str(), path.c_str()) != 0) return false;
      if (::unlink(temporary_.c_str()) != 0) return false;
    }
#endif
    temporary_.clear();return true;
  }
 private:
  fs::path temporary_;
};
void clean_stages(const fs::path& destination) {
  const auto prefix = destination.filename().string() + ".memory-tmp-";
  std::error_code ec;
  size_t visited = 0;
  for (fs::directory_iterator it(destination.parent_path(), ec); !ec && it != fs::directory_iterator() && visited++ < 4096; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (name.rfind(prefix, 0) != 0) continue;
    const auto suffix = name.substr(prefix.size());
    const auto dash = suffix.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 == suffix.size() ||
        suffix.find_first_not_of("0123456789-") != std::string::npos || suffix.find('-', dash + 1) != std::string::npos) continue;
    const auto state = it->symlink_status(ec);
    if (!ec && fs::is_regular_file(state)) fs::remove(it->path(), ec);
  }
}
bool retain(const fs::path& root, const std::string& name, const MemoryDocument& document) {
  if (!document.exists) return true;
  const auto path = archive(root, name, document.revision);
  fs::path checked_root, checked_path;
  if (!resolve(root, path.lexically_relative(root).generic_string(), &checked_root, &checked_path)) return false;
  clean_stages(path);
  MemoryDocument previous;std::string error;
  if (read(path, document.source_id, &previous, &error) != Status::Ok) return false;
  // A previous attempt can have published the name before its directory sync
  // failed. Reconfirm that boundary before replacing the authoritative head.
  if (previous.exists) return previous.revision == document.revision && sync_directory(path.parent_path());
  StagedFile staged;
  return staged.prepare(path, document.text) && staged.publish(path, false) && sync_directory(path.parent_path());
}
void prune(const fs::path& root, const std::string& name, const std::string& current, const std::string& previous, size_t keep) {
  std::vector<fs::directory_entry> entries;std::error_code ec;
  const auto path = archive(root, name, current).parent_path();
  for (fs::directory_iterator it(path, ec); !ec && it != fs::directory_iterator(); ++it) {
    if (it->is_regular_file(ec) && !it->is_symlink(ec) && it->path().extension() == ".blob") entries.push_back(*it);
    if (entries.size() > keep + 64) break;
  }
  std::vector<std::pair<fs::file_time_type, fs::path>> order;
  for (const auto& entry : entries) {
    const auto time = entry.last_write_time(ec);order.emplace_back(ec ? fs::file_time_type::min() : time, entry.path());ec.clear();
  }
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
  size_t retained = 0;
  const auto now = archive(root, name, current), before = previous == "absent" ? fs::path() : archive(root, name, previous);
  for (const auto& entry : order) {
    if (entry.second == now || entry.second == before) continue;
    if (retained++ >= keep - 2) fs::remove(entry.second, ec);
  }
}
} // namespace
bool memory_revision_valid(const std::string& value) {
  return value == "absent" || (value.size() == 71 && value.rfind("sha256:", 0) == 0 && value.find_first_not_of("0123456789abcdef", 7) == std::string::npos);
}
bool memory_document_publish_immutable(const fs::path& input, const std::string& name, const std::string& text, std::string* error) {
  fs::path root, path;
  if (text.size() > kMemoryDocumentMaxBytes || !resolve(input, name, &root, &path)) return false;
  MemoryDocument existing;
  if (read(path, source_id(root, name), &existing, error) != Status::Ok) return false;
  if (existing.exists) return existing.text == text && sync_directory(path.parent_path());
  clean_stages(path);
  StagedFile staged;
  return staged.prepare(path, text) && staged.publish(path, false) && sync_directory(path.parent_path());
}
bool memory_document_retain(const fs::path& input, const std::string& name, const MemoryDocument& document, std::string* error) {
  fs::path root, path;MemoryDocument current;
  if (!resolve(input, name, &root, &path) || read(path, source_id(root, name), &current, error) != Status::Ok ||
      current.revision != document.revision || current.source_id != document.source_id || current.text != document.text) {
    if (error && error->empty()) *error = "promotion evidence changed before retention";
    return false;
  }
  if (!retain(root, name, document)) { if(error)*error="cannot retain original promotion evidence";return false; }
  return true;
}
MemoryStoreStatus memory_document_read(const fs::path& input, const std::string& name, MemoryDocument* out, std::string* error) {
  if (error) error->clear();fs::path root, path;
  if (!resolve(input, name, &root, &path)) return fail(Status::Invalid, "invalid or linked memory source path", error);
  return read(path, source_id(root, name), out, error);
}
MemoryStoreStatus memory_document_read_revision(const fs::path& input, const std::string& name, const std::string& revision, MemoryDocument* out, std::string* error) {
  if (!out || !memory_revision_valid(revision) || revision == "absent") return fail(Status::Invalid, "invalid historical memory revision", error);
  fs::path root, path;
  if (!resolve(input, name, &root, &path)) return fail(Status::Invalid, "invalid or linked memory source path", error);
  MemoryDocument current;
  const auto live = read(path, source_id(root, name), &current, error);
  if (live != Status::Ok) return live;
  if (current.exists && current.revision == revision) { *out = std::move(current);return Status::Ok; }
  const auto original = archive(root, name, revision);
  fs::path checked_root, checked_path;
  if (!resolve(root, original.lexically_relative(root).generic_string(), &checked_root, &checked_path))
    return fail(Status::Invalid, "invalid or linked revision archive", error);
  const auto status = read(original, source_id(root, name), out, error);
  if (status != Status::Ok) return status;
  if (!out->exists) return fail(Status::Unavailable, "original memory revision is not retained", error);
  return out->revision == revision ? Status::Ok : fail(Status::Unavailable, "retained memory revision is corrupt", error);
}
struct MemoryDocumentTransaction::Impl {
  FileLock lock;
  fs::path root, path;
  std::string name;
  MemoryDocument document;
  std::function<bool()> cancelled;
  bool used = false, begun = false, published = false;
};
MemoryDocumentTransaction::MemoryDocumentTransaction() : impl_(new Impl) {}
MemoryDocumentTransaction::~MemoryDocumentTransaction() = default;
const MemoryDocument& MemoryDocumentTransaction::document() const { return impl_->document; }
bool MemoryDocumentTransaction::published() const { return impl_->published; }
MemoryStoreStatus MemoryDocumentTransaction::begin(const fs::path& input, const std::string& name, const std::optional<std::string>& expected,
    std::function<bool()> cancelled, std::string* error) {
  if (error) error->clear();
  if (impl_->used || (expected && !memory_revision_valid(*expected)) || !resolve(input, name, &impl_->root, &impl_->path))
    return fail(Status::Invalid, "invalid memory transaction or expected revision", error);
  impl_->used = true;
  impl_->name = name;impl_->cancelled = std::move(cancelled);
  const auto directory_path = impl_->root / ".locks";
  if (!directory(directory_path)) return fail(Status::Unavailable, "cannot create private memory lock directory", error);
  auto status = impl_->lock.acquire(directory_path / "writer.lock", impl_->cancelled, error);
  if (status != Status::Ok) return status;
  clean_stages(impl_->path);
  status = read(impl_->path, source_id(impl_->root, name), &impl_->document, error);
  if (status != Status::Ok) return status;
  if (expected && *expected != impl_->document.revision) return fail(Status::Conflict, "memory revision conflict; reread and reconsider the update", error);
  impl_->begun = true;return Status::Ok;
}
MemoryStoreStatus MemoryDocumentTransaction::commit(const std::string& text, std::string* error, size_t keep,
    const std::function<void(MemoryPublishPhase)>& observer) {
  if (!impl_->begun || text.size() > kMemoryDocumentMaxBytes) return fail(Status::Invalid, "memory transaction unavailable or output exceeds 8 MiB", error);
  impl_->begun = false; // one publication attempt per captured snapshot
  if (impl_->cancelled && impl_->cancelled()) return fail(Status::Cancelled, "memory update cancelled before publication", error);
  MemoryDocument next;next.exists = true;next.text = text;next.revision = digest(text);next.source_id = impl_->document.source_id;
  try {
    if (!retain(impl_->root, impl_->name, impl_->document))
      return fail(Status::Unavailable, "cannot durably retain original memory revisions; source unchanged", error);
    if (observer) observer(MemoryPublishPhase::Archived);
    if (impl_->document.exists && impl_->document.revision == next.revision) return Status::Ok;
    StagedFile staged;
    if (!staged.prepare(impl_->path, text)) return fail(Status::Unavailable, "cannot stage and sync memory document", error);
    MemoryDocument current;
    if (read(impl_->path, next.source_id, &current, error) != Status::Ok) return Status::Unavailable;
    if (current.revision != impl_->document.revision) {
      impl_->document = std::move(current);
      return fail(Status::Conflict, "memory changed outside its writer lock; source not replaced", error);
    }
    if (impl_->cancelled && impl_->cancelled()) return fail(Status::Cancelled, "memory update cancelled before replacement", error);
    if (observer) observer(MemoryPublishPhase::BeforeReplace);
    if (!staged.publish(impl_->path, true)) return fail(Status::Unavailable, "memory document replacement failed; source unchanged", error);
    const auto previous = impl_->document.revision;
    impl_->published = true;impl_->document = std::move(next);
    if (observer) observer(MemoryPublishPhase::AfterReplace);
    if (!sync_directory(impl_->path.parent_path())) return fail(Status::Uncertain, "memory replaced but directory durability is unconfirmed; inspect its revision before further writes", error);
    try { prune(impl_->root, impl_->name, impl_->document.revision, previous, std::clamp<size_t>(keep, 2, 1000)); }
    catch (...) { /* retention maintenance cannot revoke a durable publication */ }
    return Status::Ok;
  } catch (...) {
    return fail(impl_->published ? Status::Uncertain : Status::Unavailable, "memory publication interrupted; inspect its current revision", error);
  }
}
} // namespace host_tools_internal
