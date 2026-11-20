#include "memory_consolidator.h"
#include "memory_store.h"
#include "memory_promotion.h"
#include "memory_structured.h"

#include "daemon_auth.h"
#include "memory_correlation_index.h"
#include "string_util.h"

#include "toolset_host.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <vector>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string local_date_ymd_days_ago(int days_ago) {
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

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static std::string trim_ascii(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

struct ConsolidateScanFile {
  std::string layer;
  std::string rel_path;
  std::filesystem::path path;
  int64_t sort_key = 0;
};

static int64_t file_mtime_ms(const std::filesystem::path& p) {
  std::error_code ec;
  const auto ft = std::filesystem::last_write_time(p, ec);
  if (ec) return 0;
  const auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
    ft - decltype(ft)::clock::now() + std::chrono::system_clock::now()
  );
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(sys_time.time_since_epoch()).count();
}

static std::string path_rel_to(const std::filesystem::path& root, const std::filesystem::path& abs) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
  if (ec) rel = abs.lexically_relative(root);
  return rel.generic_string();
}

static bool is_markdown_file(const std::filesystem::path& p) {
  const std::string ext = to_lower_ascii(p.extension().string());
  return ext == ".md" || ext == ".markdown";
}

}  // namespace

bool memory_consolidate_once(
  const DaemonConfig& cfg,
  const MemoryConsolidateOptions& opt,
  Json::Value* out_report,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_report) *out_report = Json::Value(Json::objectValue);

  if (cfg.state_dir.empty() || cfg.sessions_root_dir.empty()) {
    if (out_err) *out_err = "missing state_dir/sessions_root_dir";
    return false;
  }
  const std::filesystem::path mem_root = std::filesystem::path(cfg.state_dir) / "memory";

  Json::Value report(Json::objectValue);
  report["schema"] = "agentd_memory_consolidate_report_v1";
  report["ts_unix_ms"] = (Json::Int64)unix_ms_now();
  report["state_dir"] = cfg.state_dir;
  report["memory_root"] = mem_root.string();
  report["dry_run"] = opt.dry_run;

  const int days = std::clamp(opt.daily_days, 0, 90);
  const int session_days = std::max(0, opt.session_days);
  const int max_session_files = std::clamp(opt.max_session_files, 0, 64);
  const int max_file_bytes = std::clamp(opt.max_file_bytes, 1024, int(host_tools_internal::kMemoryDocumentMaxBytes));
  report["include_core"] = opt.include_core;
  report["include_daily"] = opt.include_daily;
  report["include_session"] = opt.include_session;
  report["include_structured"] = opt.include_structured;
  report["daily_days"] = days;
  report["session_days"] = session_days;
  report["max_session_files"] = max_session_files;
  report["max_file_bytes"] = max_file_bytes;

  Json::Value daily_scanned(Json::arrayValue);
  Json::Value files_scanned(Json::arrayValue);
  std::vector<ConsolidateScanFile> files;

  auto add_scan_file = [&](std::string layer,
                           std::filesystem::path p,
                           std::string rel,
                           int64_t sort_key) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) return;
    ConsolidateScanFile sf;
    sf.layer = std::move(layer);
    sf.path = std::move(p);
    sf.rel_path = std::move(rel);
    sf.sort_key = sort_key;
    files.push_back(std::move(sf));
  };

  if (opt.include_core) {
    add_scan_file("core", mem_root / "MEMORY.md", "MEMORY.md", 0);
  }
  if (opt.include_structured) {
    add_scan_file("structured", mem_root / "STRUCTURED.md", "STRUCTURED.md", 10);
  }
  if (opt.include_daily) {
    // Date selects the scan window; conflicting observations never use file order as authority.
    for (int i = days - 1; i >= 0; i--) {
      const std::string ymd = local_date_ymd_days_ago(i);
      const std::filesystem::path p = mem_root / (ymd + ".md");
      std::error_code ec;
      if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        add_scan_file("daily", p, p.filename().string(), 1000000 + (days - i));
        daily_scanned.append(p.filename().string());
      }
    }
  }
  if (opt.include_session && max_session_files > 0) {
    const std::filesystem::path session_root = mem_root / "sessions";
    std::error_code ec;
    std::vector<ConsolidateScanFile> session_files;
    const int64_t cutoff_ms = session_days > 0
      ? unix_ms_now() - (int64_t)session_days * 24LL * 60 * 60 * 1000
      : 0;
    if (std::filesystem::is_symlink(session_root, ec)) {
      if (out_err) *out_err = "session memory root must not be a symlink";
      return false;
    }
    ec.clear();
    size_t visited = 0;
    if (std::filesystem::exists(session_root, ec) && std::filesystem::is_directory(session_root, ec)) {
      for (auto it = std::filesystem::recursive_directory_iterator(session_root, ec);
           !ec && it != std::filesystem::recursive_directory_iterator();
           it.increment(ec)) {
        if (++visited > 4096) { if (out_err) *out_err = "session memory walk exceeds 4096 entries"; return false; }
        const auto& de = *it;
        if (de.path().filename().string().rfind(".", 0) == 0) { it.disable_recursion_pending(); continue; }
        if (de.is_symlink(ec)) continue;
        if (!de.is_regular_file(ec)) continue;
        if (!is_markdown_file(de.path())) continue;
        const int64_t mt = file_mtime_ms(de.path());
        if (cutoff_ms > 0 && mt > 0 && mt < cutoff_ms) continue;
        ConsolidateScanFile sf;
        sf.layer = "session";
        sf.path = de.path();
        sf.rel_path = path_rel_to(mem_root, de.path());
        sf.sort_key = mt;
        session_files.push_back(std::move(sf));
      }
    }
    if (ec) { if (out_err) *out_err = "cannot enumerate session memory: " + ec.message(); return false; }
    std::sort(session_files.begin(), session_files.end(), [](const ConsolidateScanFile& a, const ConsolidateScanFile& b) {
      if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
      return a.rel_path < b.rel_path;
    });
    if ((int)session_files.size() > max_session_files) {
      session_files.erase(session_files.begin(), session_files.end() - max_session_files);
    }
    for (auto& sf : session_files) {
      sf.sort_key += 2000000;
      files.push_back(std::move(sf));
    }
  }

  std::sort(files.begin(), files.end(), [](const ConsolidateScanFile& a, const ConsolidateScanFile& b) {
    if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
    if (a.layer != b.layer) return a.layer < b.layer;
    return a.rel_path < b.rel_path;
  });
  for (const auto& sf : files) {
    Json::Value f(Json::objectValue);
    f["layer"] = sf.layer;
    f["path"] = sf.rel_path;
    files_scanned.append(f);
  }
  report["daily_files_scanned"] = daily_scanned;
  report["files_scanned"] = files_scanned;

  std::vector<std::string> paths;
  for (const auto& file : files) paths.push_back(std::filesystem::absolute(file.path).lexically_normal().string());
  host_tools_internal::MemoryPromotionPlan plan;
  if (!host_tools_internal::memory_promotion_capture(mem_root, paths, size_t(max_file_bytes),
        size_t(std::clamp(opt.max_entries, 1, 1000)), &plan, out_err)) return false;
  plan.keep_checkpoints = std::clamp(opt.keep_checkpoints, 2, 1000);
  report["marker_lines_found"] = Json::UInt64(plan.markers.size());
  std::set<std::string> keys;
  Json::Value preview(Json::arrayValue);
  for (const auto& marker : plan.markers) {
    keys.insert(marker.key);
    if (preview.size() >= 20) continue;
    Json::Value entry;
    entry["key"] = marker.key; entry["kind"] = marker.kind; entry["status"] = marker.status;
    entry["value"] = marker.value; entry["id"] = marker.id;
    entry["evidence"] = host_tools_internal::memory_passage_evidence(marker.source);
    preview.append(entry);
  }
  report["distinct_keys"] = Json::UInt64(keys.size());
  report["entries_preview"] = preview;
  if (opt.dry_run) {
    Json::Value doc;
    host_tools_internal::MemoryPromotionUpdate update;
    if (!host_tools_internal::memory_structured_parse_doc(plan.snapshot.sources.at("STRUCTURED.md").text, &doc, out_err) ||
        !host_tools_internal::memory_promotion_prepare(mem_root, doc, plan, &update, out_err, false)) return false;
    Json::Value result;
    result["dry_run"] = true; result["would_apply_keys"] = update.entries.size();
    result["would_record_receipts"] = Json::UInt64(update.new_receipts);
    result["deferred"] = Json::UInt64(update.deferred); result["errors"] = plan.errors;
    result["decisions"] = update.ledger["receipts"];
    report["promotion_preview"] = result;
    report["output"] = "dry_run: preview only; no facts or receipts published";
    if (out_report) *out_report = report;
    return host_tools_internal::memory_view_validate(plan.snapshot, out_err);
  }

  if (to_lower_ascii(trim_ascii(cfg.tools)) != "host") {
    if (out_err) *out_err = "memory consolidation requires --tools host";
    return false;
  }
  if (cfg.host_policy != HostToolsetPolicyMode::Full) {
    if (out_err) *out_err = "memory consolidation requires host_policy=full";
    return false;
  }

  HostToolsetConfig hcfg;
  hcfg.root_dir = cfg.state_dir;
  hcfg.policy = HostToolsetPolicyMode::Full;
  hcfg.enable_process_exec = false;
  hcfg.allow_symlinks = true;
  hcfg.sessions_root_dir = cfg.sessions_root_dir;
  hcfg.session_id = "";

  Json::Value resp;
  const auto status = host_tools_internal::memory_promote(hcfg, plan, &resp, out_err);
  report["memory_put_response"] = resp;
  if (out_report) *out_report = report;
  if (status != AGENT_OK || !resp.get("ok", false).asBool()) return false;
  report["promotion"] = resp["data"]["promotion"];
  if (!report["promotion"].get("complete", false).asBool()) {
    report["output"] = "promotion incomplete: inspect unresolved, deferred and parse errors; independent facts may be committed";
    if (out_report) *out_report = report;
    if (out_err) *out_err = report["output"].asString();
    return false;
  }
  report["output"] = "consolidated @mem markers into STRUCTURED.md";
  if (out_report) *out_report = report;
  return true;
}

MemoryConsolidatorEngine::MemoryConsolidatorEngine(std::function<DaemonConfig()> cfg_snapshot, Options opt)
  : cfg_snapshot_(std::move(cfg_snapshot)), opt_(opt) {}

MemoryConsolidatorEngine::~MemoryConsolidatorEngine() {
  stop();
}

bool MemoryConsolidatorEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_) return true;
  stop_ = false;
  running_ = true;
  worker_ = std::thread([this]() { worker_main(); });
  return true;
}

void MemoryConsolidatorEngine::stop() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

void MemoryConsolidatorEngine::worker_main() {
  int64_t last_run_ms = 0;
  for (;;) {
    if (stop_) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
    if (stop_) break;

    const DaemonConfig cfg = cfg_snapshot_ ? cfg_snapshot_() : DaemonConfig{};
    const int64_t interval = cfg.memory_consolidate_interval_ms;
    if (interval <= 0) continue;

    const int64_t now = unix_ms_now();
    if (last_run_ms != 0 && (now - last_run_ms) < interval) continue;

    // Only run when host tools are enabled and writable.
    if (to_lower_ascii(trim_ascii(cfg.tools)) != "host") continue;
    if (cfg.host_policy != HostToolsetPolicyMode::Full) continue;

    MemoryConsolidateOptions opt;
    opt.daily_days = cfg.memory_consolidate_daily_days;
    opt.session_days = opt.daily_days;
    opt.keep_checkpoints = cfg.memory_consolidate_keep_checkpoints;
    opt.dry_run = false;

    Json::Value rep;
    std::string err;
    const bool ok = memory_consolidate_once(cfg, opt, &rep, &err);
    if (ok) {
      MemoryCorrelationIndexOptions idx_opt = memory_correlation_index_default_options();
      MemoryCorrelationIndexReport idx_rep;
      std::string idx_err;
      const std::filesystem::path mem_root = (std::filesystem::path(cfg.state_dir) / "memory").lexically_normal();
      if (!memory_correlation_index_build(mem_root, idx_opt, &idx_rep, &idx_err)) {
        // Best-effort; consolidation stays successful even if index rebuild fails.
        if (!idx_err.empty()) {
          std::cerr << "Warning: memory correlation index rebuild failed: " << idx_err << "\n";
        }
      }
    }
    last_run_ms = now;
  }
}

}  // namespace agentd
