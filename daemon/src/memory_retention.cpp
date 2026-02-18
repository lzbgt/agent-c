#include "memory_retention.h"

#include "json_util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
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
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

bool is_daily_filename(const std::string& name, std::string* out_date, int* out_key) {
  if (out_date) out_date->clear();
  if (out_key) *out_key = 0;
  if (name.size() != 13) return false;
  if (name[4] != '-' || name[7] != '-' || name[10] != '.') return false;
  if (name.rfind(".md") != 10) return false;
  auto digit = [&](size_t i) -> int {
    if (i >= name.size()) return -1;
    const char c = name[i];
    if (c < '0' || c > '9') return -1;
    return c - '0';
  };
  int year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
  int mon = digit(5) * 10 + digit(6);
  int day = digit(8) * 10 + digit(9);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31) return false;
  if (out_date) *out_date = name.substr(0, 10);
  if (out_key) *out_key = year * 10000 + mon * 100 + day;
  return true;
}

int64_t timegm_utc(std::tm* tm) {
  if (!tm) return 0;
#if defined(_WIN32)
  return (int64_t)_mkgmtime(tm);
#else
  return (int64_t)timegm(tm);
#endif
}

bool parse_iso_utc_ms(const std::string& s, int64_t* out_unix_ms) {
  if (out_unix_ms) *out_unix_ms = 0;
  if (!out_unix_ms) return false;
  if (s.size() != 20) return false;
  auto dig2 = [&](size_t off) -> int {
    if (off + 1 >= s.size()) return -1;
    const char a = s[off];
    const char b = s[off + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
  };
  auto dig4 = [&](size_t off) -> int {
    if (off + 3 >= s.size()) return -1;
    int v = 0;
    for (size_t i = 0; i < 4; i++) {
      const char c = s[off + i];
      if (c < '0' || c > '9') return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
  const int year = dig4(0);
  const int mon = dig2(5);
  const int day = dig2(8);
  const int hour = dig2(11);
  const int min = dig2(14);
  const int sec = dig2(17);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
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
  if (t <= 0) return false;
  *out_unix_ms = t * 1000;
  return true;
}

bool read_file_bounded(const std::filesystem::path& p, size_t max_bytes, std::string* out) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  const uintmax_t sz = std::filesystem::file_size(p, ec);
  if (ec) return false;
  if (sz > max_bytes) return false;
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

void maybe_push_error(MemoryRetentionStats* stats, const std::string& msg) {
  if (!stats || msg.empty()) return;
  if (stats->errors.size() >= 50) return;
  stats->errors.push_back(msg);
}

void maybe_push_deleted(std::vector<std::string>* out, const std::string& path) {
  if (!out) return;
  if (out->size() >= 200) return;
  out->push_back(path);
}

}  // namespace

bool memory_retention_enforce(
  const DaemonConfig& cfg,
  const MemoryRetentionPolicy& policy,
  MemoryRetentionStats* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_stats) {
    if (out_error) *out_error = "missing output";
    return false;
  }
  *out_stats = MemoryRetentionStats();
  out_stats->generated_utc_ms = now_utc_ms();

  if (cfg.state_dir.empty()) {
    if (out_error) *out_error = "state_dir not configured";
    return false;
  }

  const std::filesystem::path mem_root = std::filesystem::path(cfg.state_dir) / "memory";
  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    maybe_push_error(out_stats, "memory root not found");
    return true;
  }

  struct DailyFile {
    std::filesystem::path path;
    std::string rel;
    std::string date;
    int date_key = 0;
    int64_t size = 0;
    bool keep = true;
  };
  std::vector<DailyFile> daily_files;

  for (auto it = std::filesystem::directory_iterator(mem_root, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
    const auto& de = *it;
    if (!de.is_regular_file(ec)) continue;
    const std::string fn = de.path().filename().string();
    std::string date;
    int date_key = 0;
    if (!is_daily_filename(fn, &date, &date_key)) continue;
    const auto sz = std::filesystem::file_size(de.path(), ec);
    if (ec) continue;

    DailyFile df;
    df.path = de.path();
    df.rel = de.path().lexically_relative(mem_root).generic_string();
    df.date = date;
    df.date_key = date_key;
    df.size = (int64_t)sz;
    daily_files.push_back(std::move(df));
  }

  int64_t daily_total = 0;
  for (const auto& df : daily_files) daily_total += df.size;
  out_stats->daily_bytes_before = daily_total;

  if (policy.daily_max_days > 0) {
    const int keep_days = std::max(1, policy.daily_max_days);
    const std::string cutoff = local_date_ymd_days_ago(keep_days - 1);
    for (auto& df : daily_files) {
      if (df.date < cutoff) df.keep = false;
    }
  }

  int64_t daily_after_age = 0;
  for (const auto& df : daily_files) {
    if (df.keep) daily_after_age += df.size;
  }

  if (policy.daily_max_bytes > 0 && daily_after_age > policy.daily_max_bytes) {
    std::vector<size_t> order;
    order.reserve(daily_files.size());
    for (size_t i = 0; i < daily_files.size(); i++) {
      if (daily_files[i].keep) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return daily_files[a].date_key < daily_files[b].date_key;
    });
    int64_t total = daily_after_age;
    for (size_t idx : order) {
      if (total <= policy.daily_max_bytes) break;
      DailyFile& df = daily_files[idx];
      if (!df.keep) continue;
      df.keep = false;
      total -= df.size;
    }
  }

  for (const auto& df : daily_files) {
    if (!df.keep) {
      if (!policy.dry_run) {
        std::filesystem::remove(df.path, ec);
        if (ec) {
          maybe_push_error(out_stats, std::string("failed to delete daily file: ") + df.rel);
        }
      }
      out_stats->daily_deleted_count += 1;
      maybe_push_deleted(&out_stats->daily_deleted, df.rel);
    }
  }

  int64_t daily_after = 0;
  for (const auto& df : daily_files) {
    if (df.keep) daily_after += df.size;
  }
  out_stats->daily_bytes_after = daily_after;

  struct CheckpointFile {
    std::filesystem::path path;
    std::string rel;
    int64_t ts_ms = 0;
    int64_t size = 0;
    bool keep = true;
  };
  std::vector<CheckpointFile> checkpoints;

  const std::filesystem::path ckdir = mem_root / "checkpoints";
  if (std::filesystem::exists(ckdir, ec) && std::filesystem::is_directory(ckdir, ec)) {
    for (auto it = std::filesystem::directory_iterator(ckdir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
      const auto& de = *it;
      if (!de.is_regular_file(ec)) continue;
      const std::string fn = de.path().filename().string();
      if (fn.rfind("structured_", 0) != 0) continue;
      if (fn.size() < 6 || fn.rfind(".json") != fn.size() - 5) continue;

      std::string text;
      if (!read_file_bounded(de.path(), /*max_bytes=*/10 * 1024 * 1024, &text)) {
        maybe_push_error(out_stats, std::string("failed to read checkpoint: ") + fn);
        continue;
      }
      Json::Value ck;
      std::string perr;
      if (!json_parse_any(text, &ck, &perr) || !ck.isObject()) {
        maybe_push_error(out_stats, std::string("invalid checkpoint JSON: ") + fn);
        continue;
      }
      const std::string ts_utc = ck.isMember("ts_utc") && ck["ts_utc"].isString() ? ck["ts_utc"].asString() : "";
      int64_t ts_ms = 0;
      if (!ts_utc.empty() && !parse_iso_utc_ms(ts_utc, &ts_ms)) {
        maybe_push_error(out_stats, std::string("invalid checkpoint ts_utc: ") + fn);
        continue;
      }
      if (ts_ms <= 0) {
        maybe_push_error(out_stats, std::string("missing checkpoint ts_utc: ") + fn);
        continue;
      }

      const auto sz = std::filesystem::file_size(de.path(), ec);
      if (ec) continue;

      CheckpointFile ckf;
      ckf.path = de.path();
      ckf.rel = de.path().lexically_relative(mem_root).generic_string();
      ckf.ts_ms = ts_ms;
      ckf.size = (int64_t)sz;
      checkpoints.push_back(std::move(ckf));
    }
  }

  if (policy.checkpoint_max_days > 0) {
    const int64_t cutoff_ms = out_stats->generated_utc_ms - (int64_t)policy.checkpoint_max_days * 24LL * 60 * 60 * 1000;
    for (auto& ck : checkpoints) {
      if (ck.ts_ms < cutoff_ms) ck.keep = false;
    }
  }

  if (policy.checkpoint_max_count > 0) {
    std::vector<size_t> order;
    order.reserve(checkpoints.size());
    for (size_t i = 0; i < checkpoints.size(); i++) {
      if (checkpoints[i].keep) order.push_back(i);
    }
    if ((int)order.size() > policy.checkpoint_max_count) {
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (checkpoints[a].ts_ms != checkpoints[b].ts_ms) return checkpoints[a].ts_ms < checkpoints[b].ts_ms;
        return checkpoints[a].rel < checkpoints[b].rel;
      });
      size_t keep_count = (size_t)policy.checkpoint_max_count;
      for (size_t i = 0; i < order.size(); i++) {
        if (i < keep_count) continue;
        checkpoints[order[i]].keep = false;
      }
    }
  }

  for (const auto& ck : checkpoints) {
    if (!ck.keep) {
      if (!policy.dry_run) {
        std::filesystem::remove(ck.path, ec);
        if (ec) {
          maybe_push_error(out_stats, std::string("failed to delete checkpoint: ") + ck.rel);
        }
      }
      out_stats->checkpoint_deleted_count += 1;
      maybe_push_deleted(&out_stats->checkpoint_deleted, ck.rel);
    }
  }

  return true;
}

MemoryRetentionEngine::MemoryRetentionEngine(std::function<DaemonConfig()> cfg_snapshot, Options opt)
  : cfg_snapshot_(std::move(cfg_snapshot)), opt_(opt) {}

MemoryRetentionEngine::~MemoryRetentionEngine() {
  stop();
}

bool MemoryRetentionEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_) return true;
  stop_ = false;
  running_ = true;
  worker_ = std::thread([this]() { worker_main(); });
  return true;
}

void MemoryRetentionEngine::stop() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

void MemoryRetentionEngine::worker_main() {
  int64_t last_run_ms = 0;
  for (;;) {
    if (stop_) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
    if (stop_) break;

    const DaemonConfig cfg = cfg_snapshot_ ? cfg_snapshot_() : DaemonConfig{};
    const int64_t interval = cfg.memory_retention_interval_ms;
    if (interval <= 0) continue;

    const int64_t now = now_utc_ms();
    if (last_run_ms != 0 && (now - last_run_ms) < interval) continue;

    MemoryRetentionPolicy policy;
    policy.daily_max_days = cfg.memory_retention_daily_max_days;
    policy.daily_max_bytes = cfg.memory_retention_daily_max_bytes;
    policy.checkpoint_max_days = cfg.memory_retention_checkpoint_max_days;
    policy.checkpoint_max_count = cfg.memory_retention_checkpoint_max_count;
    policy.dry_run = false;

    MemoryRetentionStats stats;
    std::string err;
    (void)memory_retention_enforce(cfg, policy, &stats, &err);
    last_run_ms = now;
  }
}

}  // namespace agentd
