#include "memory_retention.h"
#include "memory_store.h"
#include "memory_structured.h"
#include "memory_view.h"
#include <iostream>

#include "json_util.h"
#include "memory_checkpoints.h"
#include "string_util.h"
#include "toolset_host.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string local_date_ymd_days_ago(int days_ago) {
  const auto now = std::chrono::system_clock::now();
  const auto day = now - std::chrono::hours(24LL * (long long)days_ago);
  std::time_t t = std::chrono::system_clock::to_time_t(day);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

bool is_daily_filename(const std::string &name, std::string *out_date,
                       int *out_key) {
  if (out_date)
    out_date->clear();
  if (out_key)
    *out_key = 0;
  if (name.size() != 13)
    return false;
  if (name[4] != '-' || name[7] != '-' || name[10] != '.')
    return false;
  if (name.rfind(".md") != 10)
    return false;
  auto digit = [&](size_t i) -> int {
    if (i >= name.size())
      return -1;
    const char c = name[i];
    if (c < '0' || c > '9')
      return -1;
    return c - '0';
  };
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (digit(i) < 0)
      return false;
  int year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
  int mon = digit(5) * 10 + digit(6);
  int day = digit(8) * 10 + digit(9);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31)
    return false;
  const int month_days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (day > month_days[mon] + (mon == 2 && leap ? 1 : 0))
    return false;
  if (out_date)
    *out_date = name.substr(0, 10);
  if (out_key)
    *out_key = year * 10000 + mon * 100 + day;
  return true;
}

int64_t timegm_utc(std::tm *tm) {
  if (!tm)
    return 0;
#if defined(_WIN32)
  return (int64_t)_mkgmtime(tm);
#else
  return (int64_t)timegm(tm);
#endif
}

bool parse_iso_utc_ms(const std::string &s, int64_t *out_unix_ms) {
  if (out_unix_ms)
    *out_unix_ms = 0;
  if (!out_unix_ms)
    return false;
  if (s.size() != 20)
    return false;
  auto dig2 = [&](size_t off) -> int {
    if (off + 1 >= s.size())
      return -1;
    const char a = s[off];
    const char b = s[off + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9')
      return -1;
    return (a - '0') * 10 + (b - '0');
  };
  auto dig4 = [&](size_t off) -> int {
    if (off + 3 >= s.size())
      return -1;
    int v = 0;
    for (size_t i = 0; i < 4; i++) {
      const char c = s[off + i];
      if (c < '0' || c > '9')
        return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
      s[16] != ':' || s[19] != 'Z')
    return false;
  const int year = dig4(0);
  const int mon = dig2(5);
  const int day = dig2(8);
  const int hour = dig2(11);
  const int min = dig2(14);
  const int sec = dig2(17);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
    return false;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  const int64_t t = timegm_utc(&tm);
  if (t < 0)
    return false;
  char normalized[32]{};
  std::strftime(normalized, sizeof(normalized), "%Y-%m-%dT%H:%M:%SZ", &tm);
  if (s != normalized)
    return false;
  *out_unix_ms = t * 1000;
  return true;
}

void maybe_push_error(MemoryRetentionStats *stats, const std::string &msg) {
  if (!stats || msg.empty())
    return;
  if (stats->errors.size() >= 50) {
    ++stats->errors_dropped;
    return;
  }
  stats->errors.push_back(msg);
}

std::string to_lower_ascii(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
  }
  return s;
}

struct DeprecateEntry {
  std::string key;
  std::string kind;
  std::string value;
};

bool apply_structured_deprecations(const DaemonConfig &cfg,
                                   const std::vector<DeprecateEntry> &entries,
                                   const std::string &expected_revision,
                                   int keep_checkpoints,
                                   const std::function<bool()> &cancelled,
                                   Json::Value *receipt, std::string *out_err) {
  if (out_err)
    out_err->clear();
  if (entries.empty())
    return true;

  if (to_lower_ascii(trim_copy(cfg.tools)) != "host") {
    if (out_err)
      *out_err = "structured deprecate requires --tools host";
    return false;
  }
  if (cfg.host_policy != HostToolsetPolicyMode::Full) {
    if (out_err)
      *out_err = "structured deprecate requires host_policy=full";
    return false;
  }
  if (cfg.state_dir.empty() || cfg.sessions_root_dir.empty()) {
    if (out_err)
      *out_err = "missing state_dir/sessions_root_dir";
    return false;
  }

  HostToolsetConfig hcfg;
  hcfg.should_cancel = [](void *ctx) {
    return (*static_cast<const std::function<bool()> *>(ctx))();
  };
  hcfg.should_cancel_ctx = const_cast<std::function<bool()> *>(&cancelled);
  hcfg.root_dir = cfg.state_dir;
  hcfg.policy = HostToolsetPolicyMode::Full;
  hcfg.enable_process_exec = false;
  hcfg.allow_symlinks = true;
  hcfg.sessions_root_dir = cfg.sessions_root_dir;
  hcfg.session_id = "";

  agent_tool_registry_t *reg = nullptr;
  agent_tool_executor_t exec{};
  const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
  if (st != AGENT_OK || !reg || !exec.execute) {
    if (reg)
      agent_tool_registry_destroy(reg);
    toolset_host_destroy(&exec);
    if (out_err)
      *out_err = "failed to create host toolset";
    return false;
  }

  Json::Value args(Json::objectValue);
  args["path"] = "STRUCTURED.md";
  args["expected_revision"] = expected_revision;
  Json::Value arr(Json::arrayValue);
  for (const auto &e : entries) {
    Json::Value o(Json::objectValue);
    o["key"] = e.key;
    if (!e.kind.empty())
      o["kind"] = e.kind;
    o["value"] = e.value;
    o["status"] = "deprecated";
    o["source"] = "retention:auto_deprecate";
    arr.append(o);
  }
  args["entries"] = arr;
  args["checkpoint"] = true;
  args["keep_checkpoints"] = std::max(1, keep_checkpoints);

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string req = Json::writeString(wb, args);

  agent_string_t out{};
  const agent_status_t est =
      exec.execute(exec.ctx, "memory_put", req.c_str(), &out);
  const std::string response =
      out.data ? std::string(out.data, out.len) : std::string();
  agent_string_free(&out);

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);

  Json::Value parsed_receipt;
  Json::CharReaderBuilder rb;
  std::istringstream stream(response);
  std::string parse_error;
  if (!Json::parseFromStream(rb, stream, &parsed_receipt, &parse_error)) {
    if (out_err)
      *out_err = "invalid memory_put receipt";
    return false;
  }
  if (receipt)
    *receipt = parsed_receipt;
  if (est != AGENT_OK || !parsed_receipt.get("ok", false).asBool()) {
    if (out_err)
      *out_err = parsed_receipt.get("error", "memory_put failed").asString();
    return false;
  }
  return true;
}

} // namespace

namespace {
constexpr size_t kRetentionScanEntries = 4096, kRetentionMutations = 128;
constexpr size_t kRetentionCaptureBytes = 64 * 1024 * 1024;
struct RetentionSource {
  std::string path, layer;
  host_tools_internal::MemoryDocument document;
  int64_t timestamp = 0;
  bool selected = false;
};
std::string retirement_state(host_tools_internal::MemoryStoreStatus status) {
  using Status = host_tools_internal::MemoryStoreStatus;
  switch (status) {
  case Status::Ok:
    return "retired";
  case Status::Conflict:
    return "revision_conflict";
  case Status::Cancelled:
    return "cancelled";
  case Status::Busy:
    return "busy";
  case Status::Uncertain:
    return "retired_unconfirmed";
  default:
    return "not_retired";
  }
}
} // namespace

bool memory_retention_enforce(const DaemonConfig &cfg,
                              const MemoryRetentionPolicy &policy,
                              MemoryRetentionStats *stats, std::string *error) {
  using namespace host_tools_internal;
  if (error)
    error->clear();
  if (!stats) {
    if (error)
      *error = "missing retention stats";
    return false;
  }
  *stats = {};
  stats->generated_utc_ms = now_utc_ms();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto finish = [&] {
    stats->complete = stats->errors.empty() && !stats->cancelled &&
                      !stats->budget_exhausted && stats->deferred_count == 0;
    if (!stats->complete && error)
      *error = stats->errors.empty()
                   ? "retention incomplete; inspect deferred work and receipts"
                   : stats->errors.front();
    return stats->complete;
  };
  auto stopped = [&] {
    stats->cancelled =
        stats->cancelled || (policy.cancelled && policy.cancelled());
    stats->budget_exhausted =
        stats->budget_exhausted || std::chrono::steady_clock::now() >= deadline;
    return stats->cancelled || stats->budget_exhausted;
  };
  if (cfg.state_dir.empty()) {
    maybe_push_error(stats, "state_dir not configured");
    return finish();
  }
  if (policy.daily_max_days < 0 || policy.daily_max_days > 36500 ||
      policy.daily_max_bytes < 0 || policy.checkpoint_max_days < 0 ||
      policy.checkpoint_max_count < 0 || policy.structured_deprecate_days < 0 ||
      policy.structured_deprecate_max_entries < 0) {
    maybe_push_error(stats, "invalid retention policy; daily_max_days must be "
                            "0..36500 and limits nonnegative");
    return finish();
  }
  if (!policy.dry_run &&
      (cfg.tools != "host" || cfg.host_policy != HostToolsetPolicyMode::Full)) {
    maybe_push_error(
        stats, "retention writes require tools=host and host_policy=full");
    return finish();
  }
  const auto root = std::filesystem::path(cfg.state_dir) / "memory";
  size_t captured_bytes = 0, captured_files = 0;
  std::vector<RetentionSource> daily, checkpoints;
  bool hard_failure = false;
  auto collect = [&](const std::filesystem::path &dir, bool checkpoint) {
    std::error_code ec;
    const auto state = std::filesystem::symlink_status(dir, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        state.type() == std::filesystem::file_type::not_found)
      return;
    if (ec || !std::filesystem::is_directory(state) ||
        std::filesystem::is_symlink(state)) {
      maybe_push_error(stats, "invalid retention directory: " +
                                  dir.filename().string());
      hard_failure = true;
      return;
    }
    size_t visited = 0;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (stopped()) {
        hard_failure = true;
        return;
      }
      if (++visited > kRetentionScanEntries) {
        maybe_push_error(stats, "retention directory exceeds 4096 entries");
        hard_failure = true;
        stats->budget_exhausted = true;
        return;
      }
      const auto name = it->path().filename().string();
      int date_key = 0;
      std::string date;
      if (checkpoint ? !(name.rfind("structured_", 0) == 0 &&
                         it->path().extension() == ".json")
                     : !is_daily_filename(name, &date, &date_key))
        continue;
      RetentionSource source;
      source.path = it->path().lexically_relative(root).generic_string();
      source.layer = checkpoint ? "checkpoint" : "daily";
      if (++captured_files > kRetentionScanEntries) {
        stats->budget_exhausted = true;
        maybe_push_error(stats, "retention capture exceeds 4096 sources");
        hard_failure = true;
        return;
      }
      std::string why;
      if (memory_document_read(root, source.path, &source.document, &why) !=
              MemoryStoreStatus::Ok ||
          !source.document.exists) {
        maybe_push_error(
            stats, source.path + ": " +
                       (why.empty() ? "source changed during capture" : why));
        continue;
      }
      captured_bytes += source.document.text.size();
      if (captured_bytes > kRetentionCaptureBytes) {
        stats->budget_exhausted = true;
        maybe_push_error(stats, "retention capture exceeds 64 MiB");
        hard_failure = true;
        return;
      }
      if (checkpoint) {
        Json::Value doc;
        if (!json_parse_any(source.document.text, &doc, &why) ||
            !doc.isObject() || !doc["ts_utc"].isString() ||
            !parse_iso_utc_ms(doc["ts_utc"].asString(), &source.timestamp)) {
          maybe_push_error(stats, "checkpoint has no valid UTC timestamp: " +
                                      source.path);
          continue;
        }
        checkpoints.push_back(std::move(source));
      } else {
        source.timestamp = date_key;
        stats->daily_bytes_before += int64_t(source.document.text.size());
        daily.push_back(std::move(source));
      }
    }
    if (ec) {
      maybe_push_error(stats,
                       "cannot enumerate retention directory: " + ec.message());
      hard_failure = true;
    }
  };
  if (policy.daily_max_days || policy.daily_max_bytes)
    collect(root, false);
  if (!hard_failure &&
      (policy.checkpoint_max_days || policy.checkpoint_max_count))
    collect(root / "checkpoints", true);
  stats->daily_bytes_after = stats->daily_bytes_before;
  stats->daily_bytes_projected = stats->daily_bytes_before;
  if (hard_failure)
    return finish(); // Nothing was removed from an incomplete capture.
  std::sort(daily.begin(), daily.end(), [](const auto &a, const auto &b) {
    return a.timestamp < b.timestamp;
  });
  int64_t projected = stats->daily_bytes_before;
  const auto cutoff = policy.daily_max_days
                          ? local_date_ymd_days_ago(policy.daily_max_days - 1)
                          : "";
  for (auto &source : daily)
    if (!cutoff.empty() && source.path.substr(0, 10) < cutoff) {
      source.selected = true;
      projected -= int64_t(source.document.text.size());
    }
  for (auto &source : daily)
    if (!source.selected && policy.daily_max_bytes > 0 &&
        projected > policy.daily_max_bytes) {
      source.selected = true;
      projected -= int64_t(source.document.text.size());
    }
  stats->daily_bytes_projected = projected;
  std::sort(checkpoints.begin(), checkpoints.end(),
            [](const auto &a, const auto &b) {
              return a.timestamp != b.timestamp ? a.timestamp > b.timestamp
                                                : a.path < b.path;
            });
  size_t kept = 0;
  for (auto &source : checkpoints) {
    const bool old =
        policy.checkpoint_max_days > 0 &&
        source.timestamp < stats->generated_utc_ms -
                               int64_t(policy.checkpoint_max_days) * 86400000;
    source.selected = old || (policy.checkpoint_max_count > 0 &&
                              kept >= size_t(policy.checkpoint_max_count));
    if (!source.selected)
      ++kept;
  }
  for (const auto &source : daily)
    if (source.selected)
      ++stats->daily_selected_count;
  for (const auto &source : checkpoints)
    if (source.selected)
      ++stats->checkpoint_selected_count;
  std::sort(checkpoints.begin(), checkpoints.end(),
            [](const auto &a, const auto &b) {
              return a.timestamp != b.timestamp ? a.timestamp < b.timestamp
                                                : a.path < b.path;
            });
  size_t attempted = 0;
  for (auto *sources : {&daily, &checkpoints})
    for (const auto &source : *sources) {
      if (!source.selected)
        continue;
      if (stopped() || attempted >= kRetentionMutations) {
        if (attempted >= kRetentionMutations)
          stats->budget_exhausted = true;
        ++stats->deferred_count;
        continue;
      }
      ++attempted;
      Json::Value receipt;
      receipt["schema"] = "agent.memory.retirement.v1";
      receipt["path"] = source.path;
      receipt["layer"] = source.layer;
      receipt["source_id"] = source.document.source_id;
      receipt["expected_revision"] = source.document.revision;
      MemoryPassage original;
      original.path = source.path;
      original.source_id = source.document.source_id;
      original.revision = source.document.revision;
      original.end = source.document.text.size();
      receipt["original_evidence"] = memory_passage_evidence(original);
      if (policy.dry_run) {
        receipt["commit_state"] = "preview";
        stats->outcomes.append(receipt);
        continue;
      }
      std::string why;
      MemoryDocumentTransaction transaction;
      auto status = MemoryStoreStatus::Unavailable;
      try {
        if (policy.before_retire)
          policy.before_retire(source.path);
        status = transaction.begin(root, source.path, source.document.revision,
                                   stopped, &why);
        if (status == MemoryStoreStatus::Ok)
          status = transaction.retire(&why, policy.publish_observer);
      } catch (...) {
        why = "retirement preparation interrupted before removal";
      }
      receipt["commit_state"] = retirement_state(status);
      if (!transaction.document().source_id.empty())
        receipt["current_revision"] = transaction.document().revision;
      receipt["head_removed"] = transaction.published();
      if (status != MemoryStoreStatus::Ok) {
        receipt["error"] = why;
        maybe_push_error(stats, source.path + ": " + why);
        (void)stopped();
      } else if (source.layer == "daily") {
        ++stats->daily_deleted_count;
        stats->daily_bytes_after -= int64_t(source.document.text.size());
        stats->daily_deleted.push_back(source.path);
      } else {
        ++stats->checkpoint_deleted_count;
        stats->checkpoint_deleted.push_back(source.path);
      }
      stats->outcomes.append(receipt);
    }
  if (policy.structured_deprecate_days > 0) {
    MemoryDocument snapshot;
    Json::Value doc;
    std::string why;
    if (stopped())
      ++stats->deferred_count;
    else if (memory_document_read(root, "STRUCTURED.md", &snapshot, &why) !=
                 MemoryStoreStatus::Ok ||
             !memory_structured_parse_doc(snapshot.text, &doc, &why))
      maybe_push_error(stats, why);
    else {
      std::vector<DeprecateEntry> entries;
      const int limit =
          policy.structured_deprecate_max_entries > 0
              ? std::min(200, policy.structured_deprecate_max_entries)
              : 50;
      const auto cutoff_ms =
          stats->generated_utc_ms -
          int64_t(policy.structured_deprecate_days) * 86400000;
      const auto as_of = memory_view_now_utc();
      for (const auto &key : doc["items"].getMemberNames()) {
        if (stopped()) {
          ++stats->deferred_count;
          break;
        }
        const auto &record = doc["items"][key];
        if (!memory_record_current(record, as_of))
          continue;
        if (record.isMember("observed_utc") && !record["observed_utc"].isString()) {
          maybe_push_error(stats, "invalid structured observation type: " + key);
          continue;
        }
        const auto observed = record.get("observed_utc", "").asString();
        const auto updated = record.get("updated_utc", "").isString()
                                 ? record["updated_utc"].asString()
                                 : "";
        const auto timestamp = observed.empty() ? updated : observed;
        int64_t when = 0;
        if (timestamp.empty())
          continue;
        if (!parse_iso_utc_ms(timestamp, &when)) {
          maybe_push_error(stats,
                           "invalid structured observation time: " + key);
          continue;
        }
        if (when >= cutoff_ms)
          continue;
        ++stats->structured_selected_count;
        if (entries.size() >= size_t(limit)) {
          ++stats->deferred_count;
          continue;
        }
        if (!record["value"].isString()) {
          maybe_push_error(
              stats, "structured deprecation requires string value: " + key);
          continue;
        }
        entries.push_back({key, record.get("kind", "fact").asString(),
                           record["value"].asString()});
      }
      for (const auto &entry : entries)
        stats->structured_selected_keys.push_back(entry.key);
      if (!policy.dry_run && !entries.empty()) {
        if (stopped())
          stats->deferred_count += int64_t(entries.size());
        else if (!apply_structured_deprecations(
                     cfg, entries, snapshot.revision,
                     cfg.memory_consolidate_keep_checkpoints, stopped,
                     &stats->structured_write_response, &why))
          maybe_push_error(stats, why);
        else {
          stats->structured_deprecated_count = int64_t(entries.size());
          for (const auto &entry : entries)
            stats->structured_deprecated_keys.push_back(entry.key);
        }
      }
    }
  }
  return finish();
}

Json::Value memory_retention_report(const MemoryRetentionPolicy &policy,
                                    const MemoryRetentionStats &stats,
                                    const std::string &error) {
  Json::Value out;
  out["schema"] = "agent.memory.retention.v2";
  out["ok"] = stats.complete;
  out["complete"] = stats.complete;
  if (!stats.complete)
    out["error"] =
        error.empty() ? "retention incomplete; inspect receipts before recovery"
                      : error;
  out["generated_utc_ms"] = Json::Int64(stats.generated_utc_ms);
  out["dry_run"] = policy.dry_run;
  out["daily_max_days"] = policy.daily_max_days;
  out["daily_max_bytes"] = Json::Int64(policy.daily_max_bytes);
  out["checkpoint_max_days"] = policy.checkpoint_max_days;
  out["checkpoint_max_count"] = policy.checkpoint_max_count;
  out["structured_deprecate_days"] = policy.structured_deprecate_days;
  out["structured_deprecate_max_entries"] =
      policy.structured_deprecate_max_entries;
  out["daily_selected_count"] = Json::Int64(stats.daily_selected_count);
  out["checkpoint_selected_count"] =
      Json::Int64(stats.checkpoint_selected_count);
  out["structured_selected_count"] =
      Json::Int64(stats.structured_selected_count);
  out["daily_deleted_count"] = Json::Int64(stats.daily_deleted_count);
  out["checkpoint_deleted_count"] = Json::Int64(stats.checkpoint_deleted_count);
  out["structured_deprecated_count"] =
      Json::Int64(stats.structured_deprecated_count);
  out["daily_bytes_before"] = Json::Int64(stats.daily_bytes_before);
  out["daily_bytes_after"] = Json::Int64(stats.daily_bytes_after);
  out["daily_bytes_projected"] = Json::Int64(stats.daily_bytes_projected);
  out["cancelled"] = stats.cancelled;
  out["budget_exhausted"] = stats.budget_exhausted;
  out["deferred_count"] = Json::Int64(stats.deferred_count);
  out["errors_dropped"] = Json::Int64(stats.errors_dropped);
  out["outcomes"] = stats.outcomes;
  for (const auto &row :
       {std::make_pair("daily_deleted", &stats.daily_deleted),
        std::make_pair("checkpoint_deleted", &stats.checkpoint_deleted),
        std::make_pair("structured_deprecated_keys",
                       &stats.structured_deprecated_keys),
        std::make_pair("structured_selected_keys",
                       &stats.structured_selected_keys),
        std::make_pair("errors", &stats.errors)}) {
    out[row.first] = Json::Value(Json::arrayValue);
    for (const auto &value : *row.second)
      out[row.first].append(value);
  }
  if (!stats.structured_write_response.isNull())
    out["structured_write_response"] = stats.structured_write_response;
  return out;
}

MemoryRetentionEngine::MemoryRetentionEngine(
    std::function<DaemonConfig()> cfg_snapshot, Options opt)
    : cfg_snapshot_(std::move(cfg_snapshot)), opt_(opt) {}

MemoryRetentionEngine::~MemoryRetentionEngine() { stop(); }

bool MemoryRetentionEngine::start(std::string *out_error) {
  if (out_error)
    out_error->clear();
  if (running_)
    return true;
  stop_ = false;
  running_ = true;
  worker_ = std::thread([this]() { worker_main(); });
  return true;
}

void MemoryRetentionEngine::stop() {
  stop_ = true;
  if (worker_.joinable())
    worker_.join();
  running_ = false;
}

void MemoryRetentionEngine::worker_main() {
  int64_t last_run_ms = 0;
  for (;;) {
    if (stop_)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
    if (stop_)
      break;

    const DaemonConfig cfg = cfg_snapshot_ ? cfg_snapshot_() : DaemonConfig{};
    const int64_t interval = cfg.memory_retention_interval_ms;
    if (interval <= 0)
      continue;

    const int64_t now = now_utc_ms();
    if (last_run_ms != 0 && (now - last_run_ms) < interval)
      continue;

    MemoryRetentionPolicy policy;
    policy.daily_max_days = cfg.memory_retention_daily_max_days;
    policy.daily_max_bytes = cfg.memory_retention_daily_max_bytes;
    policy.checkpoint_max_days = cfg.memory_retention_checkpoint_max_days;
    policy.checkpoint_max_count = cfg.memory_retention_checkpoint_max_count;
    policy.structured_deprecate_days =
        cfg.memory_retention_structured_deprecate_days;
    policy.structured_deprecate_max_entries =
        cfg.memory_retention_structured_deprecate_max_entries;
    policy.dry_run = false;
    policy.cancelled = [this] { return stop_.load(); };

    MemoryRetentionStats stats;
    std::string err;
    if (!memory_retention_enforce(cfg, policy, &stats, &err) && !stop_) {
      std::cerr << "memory retention incomplete: " << err
                << " (confirmed daily=" << stats.daily_deleted_count
                << ", checkpoints=" << stats.checkpoint_deleted_count
                << ", deferred=" << stats.deferred_count << ")\n";
    }
    last_run_ms = now;
  }
}

} // namespace agentd
