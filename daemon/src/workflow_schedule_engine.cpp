#include "workflow_schedule_engine.h"

#include "cron_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoint_util.h"
#include "workflow_submit.h"

#include <json/json.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static std::string schedule_workflow_id(const std::string& schedule_id, int64_t tick_unix_ms) {
  std::string out = "wfs_" + schedule_id + "_" + std::to_string((long long)tick_unix_ms);
  if (out.size() > 128) out.resize(128);
  return out;
}

static std::string normalize_timezone_or_empty(const std::string& tz) {
  const std::string t = trim_copy(tz);
  if (t.empty()) return "";
  const std::string l = lower_copy(t);
  if (l == "utc" || l == "etc/utc" || l == "gmt") return "UTC";
  return "";
}

}  // namespace

WorkflowScheduleEngine::WorkflowScheduleEngine(
  AgentDb* db,
  std::function<DaemonConfig()> cfg_snapshot,
  Options opt
)
  : db_(db),
    cfg_snapshot_(std::move(cfg_snapshot)),
    opt_(opt) {}

WorkflowScheduleEngine::~WorkflowScheduleEngine() {
  stop();
}

bool WorkflowScheduleEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_) return true;
  stop_ = false;
  running_ = true;
  worker_ = std::thread([this]() { worker_main(); });
  return true;
}

void WorkflowScheduleEngine::stop() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

void WorkflowScheduleEngine::worker_main() {
  for (;;) {
    if (stop_) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
    if (stop_) break;

    if (!db_ || !db_->is_open()) continue;

    const int64_t now = now_unix_ms();
    std::vector<AgentDb::WorkflowScheduleRow> due;
    std::string derr;
    if (!db_->list_workflow_schedules_due(now, opt_.max_scan, &due, &derr)) {
      if (!derr.empty()) {
        std::cerr << "Warning: schedule scan failed: " << derr << "\n";
      }
      continue;
    }

    for (const auto& s : due) {
      if (stop_) break;
      if (s.status != "active") continue;
      if (s.next_tick_unix_ms <= 0) continue;

      if (normalize_timezone_or_empty(s.timezone).empty()) {
        std::string uerr;
        bool found = false;
        (void)db_->update_workflow_schedule_status(s.schedule_id, "error", now, &found, &uerr);
        (void)db_->update_workflow_schedule_ticks(
          s.schedule_id, s.last_tick_unix_ms, 0, "unsupported timezone", now, &found, &uerr);
        continue;
      }

      CronSchedule sched;
      std::string perr;
      if (!cron_parse_5(s.cron, &sched, &perr)) {
        std::string uerr;
        bool found = false;
        const std::string err = perr.empty() ? "invalid cron" : perr;
        (void)db_->update_workflow_schedule_status(s.schedule_id, "error", now, &found, &uerr);
        (void)db_->update_workflow_schedule_ticks(s.schedule_id, s.last_tick_unix_ms, 0, err, now, &found, &uerr);
        continue;
      }

      int64_t next_tick = 0;
      std::string nerr;
      if (!cron_next_tick_utc(sched, s.next_tick_unix_ms, &next_tick, &nerr)) {
        std::string uerr;
        bool found = false;
        const std::string err = nerr.empty() ? "failed to compute next tick" : nerr;
        (void)db_->update_workflow_schedule_status(s.schedule_id, "error", now, &found, &uerr);
        (void)db_->update_workflow_schedule_ticks(s.schedule_id, s.last_tick_unix_ms, 0, err, now, &found, &uerr);
        continue;
      }

      const int64_t tick = s.next_tick_unix_ms;
      const std::string workflow_id = schedule_workflow_id(s.schedule_id, tick);
      AgentDb::WorkflowScheduleRunRow run_row;
      run_row.schedule_id = s.schedule_id;
      run_row.tick_unix_ms = tick;
      run_row.workflow_id = workflow_id;
      run_row.created_unix_ms = now;
      run_row.status = "enqueued";
      bool inserted = false;
      std::string rerr;
      if (!db_->insert_workflow_schedule_run(run_row, &inserted, &rerr)) {
        std::cerr << "Warning: schedule run insert failed: " << rerr << "\n";
        continue;
      }

      std::string last_error;
      if (inserted) {
        Json::Value args;
        std::string jerr;
        if (!json_parse_object(s.spec_json, &args, &jerr)) {
          last_error = jerr.empty() ? "invalid schedule spec_json" : jerr;
        } else {
          args["workflow_id"] = workflow_id;
          if (!args.isMember("trace_id") || !args["trace_id"].isString() || args["trace_id"].asString().empty()) {
            args["trace_id"] = workflow_id;
          }
          args["idempotency_key"] = "schedule:" + s.schedule_id + ":" + std::to_string((long long)tick);

          HttpRequest req;
          HttpResponse resp;
          resp.status = 200;
          const DaemonConfig cfg = cfg_snapshot_ ? cfg_snapshot_() : DaemonConfig{};
          workflow_submit_handle(cfg, db_, req, std::move(args), &resp);
          if (resp.status >= 400) {
            last_error = resp.body.empty() ? "workflow submit failed" : resp.body;
          }
        }

        if (!last_error.empty()) {
          std::string uerr;
          bool found = false;
          (void)db_->update_workflow_schedule_run_status(s.schedule_id, tick, "error", last_error, &found, &uerr);
          (void)db_->update_workflow_schedule_status(s.schedule_id, "error", now, &found, &uerr);
        }
      }

      {
        std::string uerr;
        bool found = false;
        (void)db_->update_workflow_schedule_ticks(
          s.schedule_id,
          tick,
          next_tick,
          last_error,
          now,
          &found,
          &uerr
        );
      }
    }
  }
}

}  // namespace agentd
