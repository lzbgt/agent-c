#include "edge_deadline_sweeper.h"

#include "json_util.h"

#include <json/json.h>

#include <chrono>
#include <thread>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static bool is_terminal_edge_task_state(const std::string& s) {
  return s == "SUCCEEDED" || s == "FAILED" || s == "TIMED_OUT" || s == "CANCELED";
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

}  // namespace

EdgeDeadlineSweeperEngine::EdgeDeadlineSweeperEngine(AgentDb* db, std::function<DaemonConfig()> cfg_snapshot, Options opt)
  : db_(db), cfg_snapshot_(std::move(cfg_snapshot)), opt_(opt) {}

EdgeDeadlineSweeperEngine::~EdgeDeadlineSweeperEngine() {
  stop();
}

bool EdgeDeadlineSweeperEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_) return true;
  if (!db_ || !db_->is_open()) {
    if (out_error) *out_error = "db not open";
    return false;
  }
  stop_ = false;
  running_ = true;
  worker_ = std::thread([this]() { worker_main(); });
  return true;
}

void EdgeDeadlineSweeperEngine::stop() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

void EdgeDeadlineSweeperEngine::worker_main() {
  while (!stop_) {
    int poll_ms = opt_.poll_ms;
    if (cfg_snapshot_) {
      const DaemonConfig cfg = cfg_snapshot_();
      // Leave room for future config knobs; opt_ remains the hard default.
      (void)cfg;
    }
    if (poll_ms < 1) poll_ms = 1;

    const int64_t now = unix_ms_now();
    std::vector<AgentDb::EdgeTaskRow> expired;
    std::string err;
    if (db_ && db_->is_open()) {
      (void)db_->list_edge_tasks_expired_deadline(now, opt_.max_scan_rows, &expired, &err);
    }

    for (const auto& t : expired) {
      if (stop_) break;
      if (!db_ || !db_->is_open()) break;
      if (t.task_id.empty() || t.step_id.empty()) continue;

      AgentDb::EdgeTaskRow cur;
      std::string terr;
      if (!db_->get_edge_task(t.task_id, t.step_id, &cur, &terr)) continue;
      if (is_terminal_edge_task_state(cur.state)) continue;
      if (cur.deadline_utc_ms <= 0 || cur.deadline_utc_ms >= now) continue;

      cur.state = "TIMED_OUT";
      cur.updated_utc_ms = now;
      if (cur.error.empty()) cur.error = "deadline exceeded";
      (void)db_->upsert_edge_task(cur, nullptr);

      AgentDb::EdgeTaskEventRow ev;
      ev.task_id = cur.task_id;
      ev.step_id = cur.step_id;
      ev.ts_utc_ms = now;
      ev.state = "TIMED_OUT";
      Json::Value d(Json::objectValue);
      d["task_id"] = cur.task_id;
      d["step_id"] = cur.step_id;
      d["node_id"] = cur.node_id;
      d["deadline_utc_ms"] = (Json::Int64)cur.deadline_utc_ms;
      d["now_utc_ms"] = (Json::Int64)now;
      d["error"] = cur.error;
      ev.data_json = json_stringify_compact(d);
      (void)db_->insert_edge_task_event(ev, nullptr, nullptr);

      // Best-effort: reflect into edge workflow step state when task_id == workflow_id.
      AgentDb::EdgeWorkflowRow wf;
      std::string werr;
      if (db_->get_edge_workflow(cur.task_id, &wf, &werr)) {
        std::vector<AgentDb::EdgeWorkflowStepRow> steps;
        std::string serr;
        if (db_->list_edge_workflow_steps(cur.task_id, &steps, &serr)) {
          for (auto& s : steps) {
            if (s.step_id != cur.step_id) continue;
            s.state = "TIMED_OUT";
            s.updated_utc_ms = now;
            if (s.error.empty()) s.error = cur.error;
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            break;
          }
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
}

}  // namespace agentd
