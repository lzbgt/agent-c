#include "memory_salience.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

int64_t timegm_utc(std::tm* tm) {
#if defined(_WIN32)
  return (int64_t)_mkgmtime(tm);
#else
  return (int64_t)timegm(tm);
#endif
}

bool parse_iso_utc_ms(const std::string& s, int64_t* out_unix_ms) {
  if (out_unix_ms) *out_unix_ms = 0;
  if (!out_unix_ms) return false;
  if (s.size() < 10) return false;
  std::tm tm{};
  if (s.size() >= 19) {
    if (std::sscanf(s.c_str(), "%04d-%02d-%02dT%02d:%02d:%02d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
      return false;
    }
  } else {
    if (std::sscanf(s.c_str(), "%04d-%02d-%02d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
      return false;
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  const int64_t t = timegm_utc(&tm);
  if (t <= 0) return false;
  *out_unix_ms = t * 1000;
  return true;
}

bool parse_ymd_utc_ms(const std::string& ymd, int64_t* out_unix_ms) {
  if (out_unix_ms) *out_unix_ms = 0;
  if (!out_unix_ms) return false;
  if (ymd.size() != 10) return false;
  std::tm tm{};
  if (std::sscanf(ymd.c_str(), "%04d-%02d-%02d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  const int64_t t = timegm_utc(&tm);
  if (t <= 0) return false;
  *out_unix_ms = t * 1000;
  return true;
}

double decay_factor(double age_days, double half_life_days) {
  if (half_life_days <= 0.0) return 1.0;
  if (age_days <= 0.0) return 1.0;
  const double k = age_days / half_life_days;
  return std::exp(-k);
}

double structured_kind_weight(const std::string& kind) {
  const std::string k = lower_copy(trim_copy(kind));
  if (k == "fact") return 1.0;
  if (k == "pref" || k == "preference") return 0.9;
  if (k == "task") return 1.1;
  if (k == "deprecated") return 0.2;
  return 0.8;
}

bool status_is_inactive(const std::string& status) {
  const std::string s = lower_copy(trim_copy(status));
  return s == "deprecated" || s == "inactive" || s == "obsolete" || s == "disabled";
}

std::string truncate_ascii(std::string s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  if (max_chars < 3) return s.substr(0, max_chars);
  s.resize(max_chars - 3);
  s += "...";
  return s;
}

std::string json_value_to_string(const Json::Value& v) {
  if (v.isString()) return v.asString();
  if (v.isBool()) return v.asBool() ? "true" : "false";
  if (v.isInt() || v.isInt64()) return std::to_string((long long)v.asInt64());
  if (v.isUInt() || v.isUInt64()) return std::to_string((unsigned long long)v.asUInt64());
  if (v.isDouble()) {
    std::ostringstream oss;
    oss << v.asDouble();
    return oss.str();
  }
  return json_stringify(v);
}

struct Observation {
  std::string text;
  std::string ts_utc;
  int64_t ts_ms = 0;
  int importance = -1;
  int line = 1;
};

static bool line_starts_with(const std::string& s, const char* prefix) {
  if (!prefix) return false;
  const size_t n = std::strlen(prefix);
  return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

static bool parse_obs_line(const std::string& line, std::string* out_text) {
  if (!out_text) return false;
  *out_text = "";
  const char* prefixes[] = {"- @obs ", "* @obs "};
  for (const auto* p : prefixes) {
    if (line_starts_with(line, p)) {
      *out_text = trim_copy(line.substr(std::strlen(p)));
      return true;
    }
  }
  return false;
}

static bool parse_meta_kv(const std::string& line, std::string* out_key, std::string* out_val) {
  if (!out_key || !out_val) return false;
  *out_key = "";
  *out_val = "";
  if (!line_starts_with(line, "  - ")) return false;
  const std::string rest = trim_copy(line.substr(4));
  const size_t colon = rest.find(':');
  if (colon == std::string::npos) return false;
  *out_key = trim_copy(rest.substr(0, colon));
  *out_val = trim_copy(rest.substr(colon + 1));
  return !out_key->empty();
}

static std::vector<Observation> parse_daily_observations(
  const std::filesystem::path& path,
  const std::string& rel_path,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  std::vector<Observation> out;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    if (out_err) *out_err = "failed to open " + rel_path;
    return out;
  }

  Observation cur;
  bool in_obs = false;
  int line_no = 0;
  const int max_lines = 20000;
  std::string line;
  while (line_no < max_lines && std::getline(in, line)) {
    line_no++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string obs_text;
    if (parse_obs_line(line, &obs_text)) {
      if (in_obs) out.push_back(cur);
      cur = Observation{};
      cur.text = obs_text;
      cur.line = line_no;
      in_obs = true;
      continue;
    }
    if (!in_obs) continue;

    std::string key;
    std::string val;
    if (parse_meta_kv(line, &key, &val)) {
      const std::string k = lower_copy(trim_copy(key));
      if (k == "ts_utc") {
        cur.ts_utc = val;
        int64_t ts = 0;
        if (parse_iso_utc_ms(val, &ts)) cur.ts_ms = ts;
      } else if (k == "importance") {
        try {
          cur.importance = std::stoi(val);
        } catch (...) {
        }
      }
      continue;
    }
    if (line_starts_with(line, "  ")) {
      std::string extra = trim_copy(line);
      if (!extra.empty()) {
        cur.text += "\n";
        cur.text += extra;
      }
      continue;
    }
    if (line_starts_with(line, "- ") || line_starts_with(line, "* ") || line_starts_with(line, "#")) {
      out.push_back(cur);
      in_obs = false;
    }
  }
  if (in_obs) out.push_back(cur);
  return out;
}

static std::string rel_path_for(const std::filesystem::path& root, const std::filesystem::path& abs) {
  std::error_code ec;
  std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
  if (ec) rel = abs.lexically_relative(root);
  return rel.generic_string();
}

}  // namespace

bool memory_salience_collect(
  const std::filesystem::path& memory_root,
  const MemorySaliencePolicy& policy,
  MemorySalienceReport* out_report,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_report) return false;
  *out_report = MemorySalienceReport{};

  std::error_code ec;
  if (memory_root.empty() || !std::filesystem::exists(memory_root, ec) || !std::filesystem::is_directory(memory_root, ec)) {
    if (out_err) *out_err = "memory root not found";
    return false;
  }

  const int64_t now_ms = now_utc_ms();
  out_report->generated_utc_ms = now_ms;

  if (policy.include_structured) {
    std::vector<MemoryCheckpointMeta> metas;
    std::string lerr;
    if (!memory_list_structured_checkpoints(memory_root, 0, INT64_MAX, "", 1, &metas, &lerr)) {
      if (!lerr.empty()) out_report->errors.push_back(lerr);
    } else if (!metas.empty()) {
      out_report->structured_checkpoint_found = true;
      out_report->structured_checkpoint = metas[0];
      Json::Value items;
      std::string spath;
      std::string rerr;
      if (!memory_read_structured_checkpoint_items(memory_root, metas[0].checkpoint_path_rel, &spath, &items, &rerr)) {
        if (!rerr.empty()) out_report->errors.push_back(rerr);
      } else if (items.isObject()) {
        std::vector<std::string> keys = items.getMemberNames();
        for (const auto& key : keys) {
          const Json::Value rec = items[key];
          if (!rec.isObject()) continue;
          const std::string kind = rec.isMember("kind") && rec["kind"].isString() ? rec["kind"].asString() : "";
          const std::string status = rec.isMember("status") && rec["status"].isString() ? rec["status"].asString() : "";
          const std::string updated = rec.isMember("updated_utc") && rec["updated_utc"].isString() ? rec["updated_utc"].asString() : "";
          const std::string observed = rec.isMember("observed_utc") && rec["observed_utc"].isString() ? rec["observed_utc"].asString() : "";
          int64_t ts_ms = 0;
          std::string ts_utc = updated;
          if (!ts_utc.empty()) parse_iso_utc_ms(ts_utc, &ts_ms);
          if (ts_ms <= 0 && !observed.empty()) {
            ts_utc = observed;
            parse_iso_utc_ms(ts_utc, &ts_ms);
          }
          if (ts_ms <= 0) ts_ms = now_ms;
          const double age_days = std::max(0.0, (double)(now_ms - ts_ms) / (1000.0 * 60.0 * 60.0 * 24.0));
          double base = structured_kind_weight(kind);
          if (status_is_inactive(status)) base *= 0.5;
          double score = base * decay_factor(age_days, policy.half_life_days);
          MemorySalienceItem item;
          item.tier = "structured";
          item.key = key;
          item.kind = kind;
          item.status = status;
          item.score = score;
          item.ts_utc = ts_utc;
          const Json::Value val = rec.isMember("value") ? rec["value"] : Json::Value(Json::nullValue);
          item.text = truncate_ascii(trim_copy(json_value_to_string(val)), 240);
          out_report->structured_items.push_back(std::move(item));
        }
      }
    }
  }

  if (policy.include_daily) {
    const int days = std::max(0, std::min(policy.daily_days, 31));
    for (int i = 0; i < days; i++) {
      const auto now = std::chrono::system_clock::now() - std::chrono::hours(24 * std::max(0, i));
      std::time_t t = std::chrono::system_clock::to_time_t(now);
      std::tm tm{};
#if defined(_WIN32)
      localtime_s(&tm, &t);
#else
      localtime_r(&t, &tm);
#endif
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
      const std::string ymd(buf);
      const std::filesystem::path p = memory_root / (ymd + ".md");
      ec.clear();
      if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) continue;
      std::string perr;
      const std::string rel_path = rel_path_for(memory_root, p);
      std::vector<Observation> obs = parse_daily_observations(p, rel_path, &perr);
      if (!perr.empty()) out_report->errors.push_back(perr);
      int64_t file_ts_ms = 0;
      (void)parse_ymd_utc_ms(ymd, &file_ts_ms);
      for (auto& o : obs) {
        int64_t ts_ms = o.ts_ms > 0 ? o.ts_ms : (file_ts_ms > 0 ? file_ts_ms : now_ms);
        const double age_days = std::max(0.0, (double)(now_ms - ts_ms) / (1000.0 * 60.0 * 60.0 * 24.0));
        const int importance = std::min(std::max(o.importance, 0), 5);
        double base = 1.0 + policy.importance_weight * (double)importance;
        double score = base * decay_factor(age_days, policy.half_life_days);
        MemorySalienceItem item;
        item.tier = "daily";
        item.path = rel_path;
        item.line = o.line;
        item.text = truncate_ascii(trim_copy(o.text), 240);
        item.score = score;
        item.ts_utc = o.ts_utc;
        item.importance = o.importance;
        out_report->daily_items.push_back(std::move(item));
      }
    }
  }

  auto by_score = [](const MemorySalienceItem& a, const MemorySalienceItem& b) {
    if (a.score == b.score) return a.text < b.text;
    return a.score > b.score;
  };
  std::sort(out_report->structured_items.begin(), out_report->structured_items.end(), by_score);
  std::sort(out_report->daily_items.begin(), out_report->daily_items.end(), by_score);

  if ((int)out_report->structured_items.size() > policy.max_structured_items) {
    out_report->structured_items.resize((size_t)policy.max_structured_items);
  }
  if ((int)out_report->daily_items.size() > policy.max_daily_items) {
    out_report->daily_items.resize((size_t)policy.max_daily_items);
  }
  const int total = (int)out_report->structured_items.size() + (int)out_report->daily_items.size();
  if (total > policy.max_items) {
    const int keep_daily = std::max(0, policy.max_items - (int)out_report->structured_items.size());
    if ((int)out_report->daily_items.size() > keep_daily) {
      out_report->daily_items.resize((size_t)keep_daily);
    }
  }
  return true;
}

}  // namespace agentd
