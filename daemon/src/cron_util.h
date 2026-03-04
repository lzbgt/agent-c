#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

struct CronField {
  int min = 0;
  int max = 0;
  bool any = false;
  std::vector<bool> allowed;
};

struct CronSchedule {
  CronField minute;
  CronField hour;
  CronField day_of_month;
  CronField month;
  CronField day_of_week;
  bool day_of_month_any = false;
  bool day_of_week_any = false;
};

bool cron_parse_5(
  const std::string& expr,
  CronSchedule* out,
  std::string* out_error
);

bool cron_next_tick_utc(
  const CronSchedule& sched,
  int64_t after_unix_ms,
  int64_t* out_tick_unix_ms,
  std::string* out_error
);

}  // namespace agentd
