#include "cron_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {
namespace {

static std::string trim_copy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

static bool parse_int(const std::string& s, int* out) {
  if (!out) return false;
  if (s.empty()) return false;
  int sign = 1;
  size_t i = 0;
  if (s[0] == '-') {
    sign = -1;
    i = 1;
  }
  if (i >= s.size()) return false;
  int v = 0;
  for (; i < s.size(); i++) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v * sign;
  return true;
}

static bool cron_field_mark_range(
  CronField* field,
  int start,
  int end,
  int step,
  std::string* out_error
) {
  if (!field) return false;
  if (step <= 0) {
    if (out_error) *out_error = "invalid cron step";
    return false;
  }
  if (start < field->min || end > field->max || start > end) {
    if (out_error) *out_error = "cron value out of range";
    return false;
  }
  for (int v = start; v <= end; v += step) {
    field->allowed[static_cast<size_t>(v - field->min)] = true;
  }
  return true;
}

static bool cron_parse_field(
  const std::string& text,
  int min_v,
  int max_v,
  bool allow_7_for_sunday,
  CronField* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return false;
  out->min = min_v;
  out->max = max_v;
  out->any = false;
  out->allowed.assign(static_cast<size_t>(max_v - min_v + 1), false);

  const std::string raw = trim_copy(text);
  if (raw.empty()) {
    if (out_error) *out_error = "empty cron field";
    return false;
  }

  std::stringstream ss(raw);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim_copy(tok);
    if (tok.empty()) continue;
    std::string base = tok;
    int step = 1;
    const size_t slash = tok.find('/');
    if (slash != std::string::npos) {
      base = trim_copy(tok.substr(0, slash));
      const std::string step_str = trim_copy(tok.substr(slash + 1));
      if (!parse_int(step_str, &step) || step <= 0) {
        if (out_error) *out_error = "invalid cron step";
        return false;
      }
    }

    if (base == "*") {
      out->any = true;
      if (!cron_field_mark_range(out, min_v, max_v, step, out_error)) return false;
      continue;
    }

    int start = 0;
    int end = 0;
    const size_t dash = base.find('-');
    if (dash != std::string::npos) {
      const std::string a = trim_copy(base.substr(0, dash));
      const std::string b = trim_copy(base.substr(dash + 1));
      if (!parse_int(a, &start) || !parse_int(b, &end)) {
        if (out_error) *out_error = "invalid cron range";
        return false;
      }
    } else {
      if (!parse_int(base, &start)) {
        if (out_error) *out_error = "invalid cron value";
        return false;
      }
      end = start;
    }

    if (allow_7_for_sunday) {
      if (start == 7) start = 0;
      if (end == 7) end = 0;
    }

    if (!cron_field_mark_range(out, start, end, step, out_error)) return false;
  }

  return true;
}

static bool cron_match_dom_dow(
  const CronSchedule& s,
  int dom,
  int dow
) {
  const bool dom_match =
    dom >= s.day_of_month.min && dom <= s.day_of_month.max &&
    s.day_of_month.allowed[static_cast<size_t>(dom - s.day_of_month.min)];
  const bool dow_match =
    dow >= s.day_of_week.min && dow <= s.day_of_week.max &&
    s.day_of_week.allowed[static_cast<size_t>(dow - s.day_of_week.min)];

  if (s.day_of_month_any && s.day_of_week_any) return true;
  if (s.day_of_month_any) return dow_match;
  if (s.day_of_week_any) return dom_match;
  return dom_match || dow_match;
}

}  // namespace

bool cron_parse_5(
  const std::string& expr,
  CronSchedule* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return false;
  const std::string e = trim_copy(expr);
  if (e.empty()) {
    if (out_error) *out_error = "cron expression empty";
    return false;
  }

  std::vector<std::string> fields;
  {
    std::stringstream ss(e);
    std::string tok;
    while (ss >> tok) {
      fields.push_back(tok);
    }
  }
  if (fields.size() != 5) {
    if (out_error) *out_error = "cron must have 5 fields";
    return false;
  }

  if (!cron_parse_field(fields[0], 0, 59, false, &out->minute, out_error)) return false;
  if (!cron_parse_field(fields[1], 0, 23, false, &out->hour, out_error)) return false;
  if (!cron_parse_field(fields[2], 1, 31, false, &out->day_of_month, out_error)) return false;
  if (!cron_parse_field(fields[3], 1, 12, false, &out->month, out_error)) return false;
  if (!cron_parse_field(fields[4], 0, 6, true, &out->day_of_week, out_error)) return false;

  out->day_of_month_any = out->day_of_month.any;
  out->day_of_week_any = out->day_of_week.any;
  return true;
}

bool cron_next_tick_utc(
  const CronSchedule& sched,
  int64_t after_unix_ms,
  int64_t* out_tick_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_tick_unix_ms) return false;

  if (after_unix_ms < 0) after_unix_ms = 0;
  int64_t base = (after_unix_ms / 60000) * 60000;
  if (base <= after_unix_ms) base += 60000;

  const int64_t max_steps = 60LL * 24LL * 366LL; // search up to 1 year
  for (int64_t i = 0; i < max_steps; i++) {
    const int64_t ts = base + i * 60000;
    const time_t tsec = static_cast<time_t>(ts / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tsec);
#else
    gmtime_r(&tsec, &tm);
#endif

    const int minute = tm.tm_min;
    const int hour = tm.tm_hour;
    const int dom = tm.tm_mday;
    const int month = tm.tm_mon + 1;
    const int dow = tm.tm_wday;

    if (minute < sched.minute.min || minute > sched.minute.max ||
        !sched.minute.allowed[static_cast<size_t>(minute - sched.minute.min)]) continue;
    if (hour < sched.hour.min || hour > sched.hour.max ||
        !sched.hour.allowed[static_cast<size_t>(hour - sched.hour.min)]) continue;
    if (month < sched.month.min || month > sched.month.max ||
        !sched.month.allowed[static_cast<size_t>(month - sched.month.min)]) continue;
    if (!cron_match_dom_dow(sched, dom, dow)) continue;

    *out_tick_unix_ms = ts;
    return true;
  }

  if (out_error) *out_error = "no matching cron tick within 366 days";
  return false;
}

}  // namespace agentd
