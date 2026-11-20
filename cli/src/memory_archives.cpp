#if defined(AGENT_HAVE_JSONCPP)
#include "memory_archives.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
namespace host_tools_internal {
namespace {
namespace fs = std::filesystem;
constexpr size_t kEntries = 4096, kBytes = 64 * 1024 * 1024, kPins = 20000;
bool hex(const std::string &text) {
  return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string compact(const Json::Value &value) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, value);
}
struct Candidate {
  std::string path, revision, intent_path;
  fs::file_time_type mtime;
  uint64_t bytes = 0;
};
} // namespace
Json::Value memory_archive_report(const MemoryArchiveReport &r) {
  Json::Value out;
  out["schema"] = "agent.memory.archive_expiry.v1";
  out["complete"] = r.complete;
  out["cancelled"] = r.cancelled;
  out["budget_exhausted"] = r.budget_exhausted;
  out["selected"] = Json::UInt64(r.selected);
  out["protected_count"] = Json::UInt64(r.protected_count);
  out["expired"] = Json::UInt64(r.expired);
  out["uncertain"] = Json::UInt64(r.uncertain);
  out["deferred"] = Json::UInt64(r.deferred);
  out["expired_bytes"] = Json::UInt64(r.expired_bytes);
  out["outcomes"] = r.outcomes;
  out["errors"] = r.errors;
  out["errors_dropped"] = Json::UInt64(r.errors_dropped);
  return out;
}
bool memory_archive_expire(const fs::path &root, const MemoryArchivePolicy &policy,
                           MemoryArchiveReport *out, std::string *error) {
  if (error)
    error->clear();
  if (!out)
    return false;
  *out = {};
  auto &r = *out;
  auto fail = [&](const std::string &reason) {
    r.complete = false;
    if (r.errors.size() < 50)
      r.errors.append(reason);
    else
      ++r.errors_dropped;
    if (error)
      *error = reason;
    return false;
  };
  auto done = [&]() {
    r.complete = r.errors.empty() && !r.cancelled && !r.budget_exhausted && r.deferred == 0;
    if (!r.complete && error && error->empty())
      *error = "archive expiry incomplete; inspect outcomes";
    return r.complete;
  };
  const auto local_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const auto deadline = policy.deadline == std::chrono::steady_clock::time_point{}
                            ? local_deadline
                            : std::min(local_deadline, policy.deadline);
  auto stopped = [&]() {
    r.cancelled = r.cancelled || (policy.cancelled && policy.cancelled());
    r.budget_exhausted = r.budget_exhausted || std::chrono::steady_clock::now() >= deadline;
    return r.cancelled || r.budget_exhausted;
  };
  if (root.empty())
    return fail("memory root is required");
  if (policy.max_age_days < 0 || policy.max_age_days > 36500 || policy.max_expirations > 128)
    return fail("invalid archive expiry age or operation budget");
  if (policy.max_age_days == 0)
    return true;
  std::error_code ec;
  const auto state = fs::symlink_status(root / ".revisions", ec);
  if (ec == std::errc::no_such_file_or_directory || state.type() == fs::file_type::not_found)
    return true;
  if (ec || !fs::is_directory(state) || fs::is_symlink(state))
    return fail("invalid revision archive directory");
  MemoryDocumentTransaction guard;
  if (!policy.dry_run &&
      guard.begin(root, "STRUCTURED.md", std::nullopt, stopped, error) != MemoryStoreStatus::Ok)
    return fail(error ? *error : "cannot acquire memory writer");
  std::map<std::string, MemoryDocument> heads;
  std::set<std::string> pins = policy.protected_revisions;
  if (pins.size() > kPins) {
    r.budget_exhausted = true;
    return fail("archive receipt pins exceed 20000 revisions");
  }
  for (const auto &pin : pins)
    if (!memory_revision_valid(pin) || pin == "absent")
      return fail("invalid archive receipt pin");
  size_t entries = 0, bytes = 0, tombstones = 0;
  bool valid = true;
  std::function<void(const fs::path &, int)> capture = [&](const fs::path &directory, int depth) {
    if (!valid)
      return;
    if (depth > 8) {
      valid = fail("active memory traversal exceeds depth 8");
      return;
    }
    std::error_code issue;
    for (auto it = fs::directory_iterator(directory, issue);
         !issue && it != fs::directory_iterator(); it.increment(issue)) {
      if (stopped()) {
        valid = false;
        return;
      }
      if (++entries > kEntries) {
        r.budget_exhausted = true;
        valid = fail("active memory directory capture exceeds 4096 entries");
        return;
      }
      const auto name = it->path().filename().string();
      if (name.empty() || name[0] == '.')
        continue;
      const auto type = it->symlink_status(issue);
      if (issue)
        break;
      if (fs::is_symlink(type)) {
        valid = fail("linked active memory source prevents expiry");
        return;
      }
      if (fs::is_directory(type)) {
        capture(it->path(), depth + 1);
        if (!valid)
          return;
        continue;
      }
      auto extension = it->path().extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
      if (extension != ".md" && extension != ".markdown" && extension != ".json")
        continue;
      const auto path = it->path().lexically_relative(root).generic_string();
      MemoryDocument document;
      std::string why;
      if (memory_document_read(root, path, &document, &why) != MemoryStoreStatus::Ok ||
          !document.exists) {
        valid = fail("active memory capture failed: " + path + ": " + why);
        return;
      }
      bytes += document.text.size();
      if (bytes > kBytes) {
        r.budget_exhausted = true;
        valid = fail("archive pin capture exceeds 64 MiB");
        return;
      }
      pins.insert(document.revision);
      if (pins.size() > kPins) {
        r.budget_exhausted = true;
        valid = fail("archive pin capture exceeds 20000 revisions");
        return;
      }
      size_t pos = 0;
      while ((pos = document.text.find("sha256:", pos)) != std::string::npos) {
        const auto value = document.text.substr(pos + 7, 64);
        if (hex(value))
          pins.insert("sha256:" + value);
        pos += 7;
        if (pins.size() > kPins) {
          r.budget_exhausted = true;
          valid = fail("archive pin capture exceeds 20000 revisions");
          return;
        }
      }
      heads.emplace(path, std::move(document));
    }
    if (issue)
      valid = fail("cannot enumerate active memory documents");
  };
  capture(root, 0);
  if (!valid)
    return done();
  // Existing metadata intents remain bounded; history is not deleted to make room.
  entries = 0;
  auto count_intents = [&]() {
    std::error_code issue;
    auto type = fs::symlink_status(root / ".expiry", issue);
    if (issue == std::errc::no_such_file_or_directory || type.type() == fs::file_type::not_found)
      return true;
    if (issue || !fs::is_directory(type) || fs::is_symlink(type))
      return fail("invalid archive expiry intent directory");
    for (auto it = fs::recursive_directory_iterator(root / ".expiry", issue);
         !issue && it != fs::recursive_directory_iterator(); it.increment(issue)) {
      if (it.depth() > 2)
        return fail("invalid expiry intent depth");
      if (stopped())
        return false;
      if (++entries > kEntries) {
        r.budget_exhausted = true;
        return fail("archive expiry intents exceed 4096 entries");
      }
      const auto status = it->symlink_status(issue);
      if (issue || fs::is_symlink(status))
        return fail("invalid linked archive expiry intent");
      if (fs::is_regular_file(status))
        ++tombstones;
    }
    return !issue || fail("cannot enumerate archive expiry intents");
  };
  if (!count_intents())
    return done();
  entries = 0;
  std::vector<Candidate> candidates;
  const auto cutoff =
      fs::file_time_type::clock::now() - std::chrono::hours(24LL * policy.max_age_days);
  for (auto bucket = fs::directory_iterator(root / ".revisions", ec);
       !ec && bucket != fs::directory_iterator(); bucket.increment(ec)) {
    if (stopped())
      return done();
    if (++entries > kEntries) {
      r.budget_exhausted = true;
      return fail("revision archive capture exceeds 4096 entries");
    }
    const auto name = bucket->path().filename().string();
    if (!hex(name))
      continue;
    const auto kind = bucket->symlink_status(ec);
    if (ec)
      break;
    if (!fs::is_directory(kind) || fs::is_symlink(kind)) {
      fail("invalid archive bucket: " + name);
      continue;
    }
    std::error_code issue;
    for (auto it = fs::directory_iterator(bucket->path(), issue);
         !issue && it != fs::directory_iterator(); it.increment(issue)) {
      if (stopped())
        return done();
      if (++entries > kEntries) {
        r.budget_exhausted = true;
        return fail("revision archive capture exceeds 4096 entries");
      }
      const auto leaf = it->path().filename().string();
      if (leaf.size() != 69 || leaf.substr(64) != ".blob" || !hex(leaf.substr(0, 64)))
        continue;
      const auto status = it->symlink_status(issue);
      if (issue)
        break;
      if (!fs::is_regular_file(status) || fs::is_symlink(status)) {
        fail("invalid archive object: " + leaf);
        continue;
      }
      Candidate row;
      row.path = it->path().lexically_relative(root).generic_string();
      row.revision = "sha256:" + leaf.substr(0, 64);
      row.mtime = it->last_write_time(issue);
      if (issue)
        break;
      row.bytes = it->file_size(issue);
      if (issue)
        break;
      if (pins.count(row.revision)) {
        ++r.protected_count;
        continue;
      }
      if (row.mtime >= cutoff)
        continue;
      row.intent_path = ".expiry/" + name + "/" + leaf.substr(0, 64) + ".json";
      candidates.push_back(std::move(row));
    }
    if (issue)
      return fail("incomplete archive bucket capture");
  }
  if (ec)
    return fail("incomplete revision archive capture");
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
  });
  r.selected = candidates.size();
  auto validate_heads = [&]() {
    for (const auto &head : heads) {
      MemoryDocument current;
      std::string why;
      if (memory_document_read(root, head.first, &current, &why) != MemoryStoreStatus::Ok ||
          current.revision != head.second.revision)
        return fail("memory changed during archive expiry; originals preserved");
    }
    return true;
  };
  if (!validate_heads())
    return done();
  size_t attempted = 0, processed = 0;
  for (const auto &row : candidates) {
    ++processed;
    if (stopped() || attempted >= policy.max_expirations || bytes + row.bytes > kBytes) {
      ++r.deferred;
      if (attempted >= policy.max_expirations || bytes + row.bytes > kBytes)
        r.budget_exhausted = true;
      continue;
    }
    ++attempted;
    bytes += row.bytes;
    Json::Value outcome;
    outcome["archive_path"] = row.path;
    outcome["revision"] = row.revision;
    outcome["bytes"] = Json::UInt64(row.bytes);
    outcome["intent_path"] = row.intent_path;
    MemoryDocument original;
    std::string why;
    auto status = memory_document_read(root, row.path, &original, &why);
    if (status != MemoryStoreStatus::Ok || !original.exists || original.revision != row.revision) {
      outcome["commit_state"] = "not_expired";
      outcome["error"] = "archive missing, corrupt or changed";
      r.outcomes.append(outcome);
      fail("archive missing, corrupt or changed: " + row.path);
      continue;
    }
    if (policy.before_expire && !policy.dry_run) {
      try {
        policy.before_expire(row.path);
      } catch (...) {
        fail("archive expiry preparation interrupted; original preserved");
        outcome["commit_state"] = "not_expired";
        outcome["error"] = "preparation interrupted";
        r.outcomes.append(outcome);
        continue;
      }
      if (!validate_heads()) {
        r.deferred += candidates.size() - processed + 1;
        break;
      }
    }
    MemoryDocument prior;
    status = memory_document_read(root, row.intent_path, &prior, &why);
    if (status != MemoryStoreStatus::Ok) {
      fail(why);
      outcome["commit_state"] = "not_expired";
      outcome["error"] = why;
      r.outcomes.append(outcome);
      continue;
    }
    Json::Value intent;
    intent["schema"] = "agent.memory.expiry_intent.v1";
    intent["state"] = "prepared";
    intent["archive_path"] = row.path;
    intent["revision"] = row.revision;
    intent["bytes"] = Json::UInt64(row.bytes);
    intent["max_age_days"] = policy.max_age_days;
    auto intent_text = compact(intent) + "\n";
    if (prior.exists) {
      if (prior.text.size() > 16384) {
        fail("expiry intent exceeds 16 KiB");
        outcome["commit_state"] = "not_expired";
        outcome["error"] = "invalid original expiry intent size";
        r.outcomes.append(outcome);
        continue;
      }
      Json::Value parsed;
      Json::CharReaderBuilder reader;
      reader["rejectDupKeys"] = true;
      reader["failIfExtra"] = true;
      reader["allowComments"] = false;
      std::istringstream input(prior.text);
      if (!Json::parseFromStream(reader, input, &parsed, &why) || !parsed.isObject() ||
          parsed["schema"] != "agent.memory.expiry_intent.v1" || parsed["state"] != "prepared" ||
          parsed["archive_path"] != row.path || parsed["revision"] != row.revision ||
          !parsed["bytes"].isUInt64() || parsed["bytes"].asUInt64() != row.bytes) {
        fail("invalid original expiry intent");
        outcome["commit_state"] = "not_expired";
        outcome["error"] = "invalid original expiry intent";
        r.outcomes.append(outcome);
        continue;
      }
      intent_text = prior.text;
    } else if (tombstones >= 2048) {
      r.budget_exhausted = true;
      ++r.deferred;
      continue;
    } else
      ++tombstones;
    if (policy.dry_run) {
      outcome["commit_state"] = "preview";
      r.outcomes.append(outcome);
      continue;
    }
    bool removed = false;
    status = guard.expire_revision_blob(row.path, row.revision, intent_text, &removed, &why,
                                        policy.observer);
    outcome["archive_removed"] = removed;
    outcome["commit_state"] = status == MemoryStoreStatus::Ok          ? "expired"
                              : status == MemoryStoreStatus::Uncertain ? "expired_unconfirmed"
                                                                       : "not_expired";
    if (status == MemoryStoreStatus::Ok) {
      ++r.expired;
      r.expired_bytes += row.bytes;
    } else {
      if (removed)
        ++r.uncertain;
      outcome["error"] = why;
      fail(why);
    }
    r.outcomes.append(outcome);
  }
  if (!validate_heads())
    return done();
  (void)stopped();
  return done();
}
} // namespace host_tools_internal
#endif
