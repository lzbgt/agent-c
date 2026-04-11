#include "memory_consolidator.h"

#include "daemon_auth.h"
#include "memory_correlation_index.h"
#include "string_util.h"

#include "toolset_host.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
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

static bool starts_with_ci(const std::string& s, const char* prefix) {
  if (!prefix) return false;
  const size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  return to_lower_ascii(s.substr(0, n)) == to_lower_ascii(prefix);
}

struct MarkerEntry {
  std::string key;
  std::string kind;
  std::string value;
  std::string status;
  std::string source;
};

struct ConsolidateScanFile {
  std::string layer;
  std::string rel_path;
  std::string source_prefix;
  std::filesystem::path path;
  int64_t sort_key = 0;
  bool skip_structured_machine_block = false;
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

static bool parse_mem_marker_line(const std::string& line_in, std::string* out_kind, std::string* out_status, std::string* out_key, std::string* out_value) {
  if (out_kind) out_kind->clear();
  if (out_status) out_status->clear();
  if (out_key) out_key->clear();
  if (out_value) out_value->clear();

  std::string s = trim_ascii(line_in);
  if (s.empty()) return false;
  // Allow leading bullet markers.
  if (s[0] == '-' || s[0] == '*' || s[0] == '+') {
    s = trim_ascii(s.substr(1));
  }
  if (s.empty()) return false;
  if (!starts_with_ci(s, "@mem") && !starts_with_ci(s, "@memory")) return false;

  // Strip @mem / @memory.
  size_t p = s.find(' ');
  if (p == std::string::npos) return false;
  s = trim_ascii(s.substr(p + 1));
  if (s.empty()) return false;

  // token1: kind or status
  size_t sp = s.find(' ');
  if (sp == std::string::npos) return false;
  std::string t1 = trim_ascii(s.substr(0, sp));
  std::string rest = trim_ascii(s.substr(sp + 1));
  if (t1.empty() || rest.empty()) return false;

  // Split key/value by '='
  const size_t eq = rest.find('=');
  if (eq == std::string::npos) return false;
  std::string key = trim_ascii(rest.substr(0, eq));
  std::string value = trim_ascii(rest.substr(eq + 1));
  if (key.empty() || value.empty()) return false;
  if (key.size() > 200) return false;

  const std::string lt1 = to_lower_ascii(t1);
  std::string kind = "fact";
  std::string status = "active";
  if (lt1 == "fact") kind = "fact";
  else if (lt1 == "pref" || lt1 == "preference") kind = "preference";
  else if (lt1 == "task") kind = "task";
  else if (lt1 == "deprecated") {
    kind = "fact";
    status = "deprecated";
  } else {
    return false;
  }

  if (out_kind) *out_kind = kind;
  if (out_status) *out_status = status;
  if (out_key) *out_key = key;
  if (out_value) *out_value = value;
  return true;
}

static bool read_file_bounded(const std::filesystem::path& p, int max_bytes, std::string* out) {
  if (!out) return false;
  out->clear();
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  if ((int)out->size() > max_bytes) out->resize((size_t)max_bytes);
  return true;
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

  const int days = std::max(0, opt.daily_days);
  const int session_days = std::max(0, opt.session_days);
  const int max_session_files = std::max(0, opt.max_session_files);
  const int max_file_bytes = std::max(1024, opt.max_file_bytes);
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
                           int64_t sort_key,
                           bool skip_structured_machine_block) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) return;
    ConsolidateScanFile sf;
    sf.layer = std::move(layer);
    sf.path = std::move(p);
    sf.rel_path = std::move(rel);
    sf.source_prefix = sf.layer + ":" + sf.rel_path;
    sf.sort_key = sort_key;
    sf.skip_structured_machine_block = skip_structured_machine_block;
    files.push_back(std::move(sf));
  };

  if (opt.include_core) {
    add_scan_file("core", mem_root / "MEMORY.md", "MEMORY.md", 0, false);
  }
  if (opt.include_structured) {
    add_scan_file("structured", mem_root / "STRUCTURED.md", "STRUCTURED.md", 10, true);
  }
  if (opt.include_daily) {
    // Scan oldest -> newest so "last write wins" maps to newest.
    for (int i = days - 1; i >= 0; i--) {
      const std::string ymd = local_date_ymd_days_ago(i);
      const std::filesystem::path p = mem_root / (ymd + ".md");
      std::error_code ec;
      if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        add_scan_file("daily", p, p.filename().string(), 1000000 + (days - i), false);
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
    if (std::filesystem::exists(session_root, ec) && std::filesystem::is_directory(session_root, ec)) {
      for (auto it = std::filesystem::recursive_directory_iterator(session_root, ec);
           !ec && it != std::filesystem::recursive_directory_iterator();
           ++it) {
        const auto& de = *it;
        if (de.is_symlink(ec)) continue;
        if (!de.is_regular_file(ec)) continue;
        if (!is_markdown_file(de.path())) continue;
        const int64_t mt = file_mtime_ms(de.path());
        if (cutoff_ms > 0 && mt > 0 && mt < cutoff_ms) continue;
        ConsolidateScanFile sf;
        sf.layer = "session";
        sf.path = de.path();
        sf.rel_path = path_rel_to(mem_root, de.path());
        sf.source_prefix = sf.layer + ":" + sf.rel_path;
        sf.sort_key = mt;
        sf.skip_structured_machine_block = false;
        session_files.push_back(std::move(sf));
      }
    }
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

  std::vector<std::string> keys_in_order;
  std::unordered_map<std::string, size_t> key_to_index;
  std::vector<MarkerEntry> entries;
  int marker_lines = 0;

  for (const auto& sf : files) {
    std::string content;
    if (!read_file_bounded(sf.path, max_file_bytes, &content)) continue;

    std::istringstream iss(content);
    std::string line;
    int line_no = 0;
    bool in_structured_machine_block = false;
    while (std::getline(iss, line)) {
      line_no++;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (sf.skip_structured_machine_block) {
        if (line.find("<!-- AGENT_MEMORY_V1_BEGIN -->") != std::string::npos) {
          in_structured_machine_block = true;
          continue;
        }
        if (line.find("<!-- AGENT_MEMORY_V1_END -->") != std::string::npos) {
          in_structured_machine_block = false;
          continue;
        }
        if (in_structured_machine_block) continue;
      }
      std::string kind, status, key, value;
      if (!parse_mem_marker_line(line, &kind, &status, &key, &value)) continue;
      marker_lines++;

      MarkerEntry e;
      e.key = key;
      e.kind = kind;
      e.status = status;
      e.value = value;
      e.source = sf.layer == "structured"
        ? sf.source_prefix
        : sf.source_prefix + "#L" + std::to_string(line_no);

      auto it = key_to_index.find(e.key);
      if (it == key_to_index.end()) {
        key_to_index[e.key] = entries.size();
        keys_in_order.push_back(e.key);
        entries.push_back(std::move(e));
      } else {
        entries[it->second] = std::move(e);
      }
      if ((int)entries.size() >= opt.max_entries) break;
    }
    if ((int)entries.size() >= opt.max_entries) break;
  }

  report["marker_lines_found"] = marker_lines;
  report["distinct_keys"] = (Json::Int64)entries.size();

  Json::Value preview(Json::arrayValue);
  for (size_t i = 0; i < entries.size() && i < 20; i++) {
    Json::Value o(Json::objectValue);
    o["key"] = entries[i].key;
    o["kind"] = entries[i].kind;
    o["status"] = entries[i].status;
    o["value"] = entries[i].value;
    o["source"] = entries[i].source;
    preview.append(o);
  }
  report["entries_preview"] = preview;

  if (entries.empty()) {
    report["output"] = "no @mem markers found";
    if (out_report) *out_report = report;
    return true;
  }

  if (opt.dry_run) {
    report["output"] = "dry_run: would apply markers into STRUCTURED.md";
    if (out_report) *out_report = report;
    return true;
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

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
  if (st != AGENT_OK || !reg || !exec.execute) {
    if (reg) agent_tool_registry_destroy(reg);
    toolset_host_destroy(&exec);
    if (out_err) *out_err = "failed to create host toolset";
    return false;
  }

  Json::Value args(Json::objectValue);
  args["path"] = "STRUCTURED.md";
  Json::Value arr(Json::arrayValue);
  for (const auto& e : entries) {
    Json::Value o(Json::objectValue);
    o["key"] = e.key;
    o["kind"] = e.kind;
    o["value"] = e.value;
    o["status"] = e.status;
    o["source"] = e.source;
    arr.append(o);
  }
  args["entries"] = arr;
  args["checkpoint"] = true;
  args["keep_checkpoints"] = std::max(1, opt.keep_checkpoints);

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string req = Json::writeString(wb, args);

  agent_string_t out{};
  const agent_status_t est = exec.execute(exec.ctx, "memory_put", req.c_str(), &out);
  const std::string resp_s = (out.data && out.len) ? std::string(out.data, out.len) : std::string();
  agent_string_free(&out);

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);

  if (est != AGENT_OK) {
    if (out_err) *out_err = "memory_put failed";
    return false;
  }

  Json::Value resp(Json::objectValue);
  {
    Json::CharReaderBuilder rb;
    std::string perr;
    std::istringstream iss(resp_s);
    if (!Json::parseFromStream(rb, iss, &resp, &perr)) {
      if (out_err) *out_err = "failed to parse memory_put response: " + perr;
      return false;
    }
  }

  report["memory_put_response"] = resp;
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
