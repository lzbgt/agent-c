#include "memory_salience.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace agentd {
namespace {
int64_t utc_ms(const std::string &text) {
  std::tm value{};
  std::istringstream stream(text);
  stream >> std::get_time(&value, "%Y-%m-%dT%H:%M:%SZ");
  if (stream.fail())
    return 0;
#if defined(_WIN32)
  return int64_t(_mkgmtime(&value)) * 1000;
#else
  return int64_t(timegm(&value)) * 1000;
#endif
}
} // namespace
void memory_salience_rank(const host_tools_internal::MemoryViewSnapshot &snapshot,
                          const MemorySaliencePolicy &policy, MemorySalienceReport *report) {
  using namespace host_tools_internal;
  *report = {};
  report->snapshot = snapshot;
  report->generated_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
  for (const auto &passage : snapshot.passages) {
    const bool structured = !passage.pointer.empty();
    const bool daily = !structured && passage.kind == "observation" && passage.path.size() == 13 &&
                       passage.path.substr(10) == ".md";
    if ((structured && !policy.include_structured) || (!structured && (!daily || !policy.include_daily)))
      continue;
    // Hard exclusion precedes scoring, even if the supplied snapshot was broader.
    if (policy.view == MemoryViewMode::Current &&
        (passage.historical || !memory_record_current(passage.record, snapshot.as_of_utc)))
      continue;
    MemorySalienceItem item;
    item.tier = structured ? "structured" : "daily";
    item.path = passage.path;
    item.line = passage.line;
    item.key = passage.key;
    item.kind = passage.kind;
    item.status = passage.status;
    item.historical = passage.historical;
    item.evidence = memory_passage_evidence(passage);
    item.supporting_evidence = passage.supporting_evidence;
    const auto &rec = passage.record;
    item.text = rec["value"].isString() ? memory_view_clip(rec["value"].asString(), 240)
                                        : memory_view_clip(passage.text, 240);
    item.ts_utc = rec["observed_utc"].isString()  ? rec["observed_utc"].asString()
                  : rec["updated_utc"].isString() ? rec["updated_utc"].asString()
                                                  : "";
    const auto observed = utc_ms(item.ts_utc);
    const double age_days = observed > 0
                                ? std::max(0.0, double(report->generated_utc_ms - observed) / 86400000.0)
                                : std::max(.01, policy.half_life_days);
    item.importance = rec["importance"].isInt() ? std::clamp(rec["importance"].asInt(), 0, 5) : -1;
    const double weight =
        structured ? (item.kind == "task"                                ? 1.1
                      : item.kind == "preference" || item.kind == "pref" ? .9
                                                                         : 1.0)
                   : 1.0 + std::clamp(policy.importance_weight, 0.0, 10.0) * std::max(0, item.importance);
    item.score = weight * std::exp(-age_days / std::max(.01, policy.half_life_days));
    if (!std::isfinite(item.score))
      continue;
    (structured ? report->structured_items : report->daily_items).push_back(std::move(item));
  }
  auto bound = [](std::vector<MemorySalienceItem> *items, int maximum) {
    std::sort(items->begin(), items->end(), [](const auto &a, const auto &b) {
      return a.score != b.score ? a.score > b.score
                                : a.evidence["id"].asString() < b.evidence["id"].asString();
    });
    if (items->size() > size_t(std::max(0, maximum)))
      items->resize(size_t(std::max(0, maximum)));
  };
  bound(&report->structured_items, policy.max_structured_items);
  bound(&report->daily_items, policy.max_daily_items);
  if (report->structured_items.size() > size_t(std::max(0, policy.max_items)))
    report->structured_items.resize(size_t(std::max(0, policy.max_items)));
  bound(&report->daily_items, std::max(0, policy.max_items - int(report->structured_items.size())));
}
bool memory_salience_collect(const std::filesystem::path &root, const MemorySaliencePolicy &policy,
                             MemorySalienceReport *report, std::string *error) {
  using namespace host_tools_internal;
  if (!report)
    return false;
  *report = {};
  std::vector<std::string> files;
  if (policy.include_structured)
    files.push_back((root / "STRUCTURED.md").string());
  if (policy.include_daily)
    for (int i = 0; i < std::clamp(policy.daily_days, 0, 31); ++i) {
      const auto time = std::time(nullptr) - 86400 * i;
      std::tm utc{};
#if defined(_WIN32)
      gmtime_s(&utc, &time);
#else
      gmtime_r(&time, &utc);
#endif
      char date[32]{};
      std::strftime(date, sizeof(date), "%Y-%m-%d.md", &utc);
      files.push_back((root / date).string());
    }
  MemoryViewSnapshot snapshot;
  if (!memory_view_capture(root, files, policy.view, &snapshot, error))
    return false;
  memory_salience_rank(snapshot, policy, report);
  if (!memory_view_validate(snapshot, error)) {
    *report = {};
    return false;
  }
  return true;
}
} // namespace agentd
