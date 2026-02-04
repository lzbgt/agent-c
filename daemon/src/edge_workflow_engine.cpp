#include "edge_workflow_engine.h"

#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static bool parse_string_array_json(const std::string& json, std::vector<std::string>* out) {
  if (!out) return false;
  out->clear();
  if (json.empty()) return true;
  Json::Value v;
  std::string err;
  if (!json_parse_any(json, &v, &err) || !v.isArray()) return false;
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (v[i].isString()) out->push_back(v[i].asString());
  }
  return true;
}

static bool parse_string_set_json(const std::string& json, std::unordered_set<std::string>* out) {
  if (!out) return false;
  out->clear();
  std::vector<std::string> tmp;
  if (!parse_string_array_json(json, &tmp)) return false;
  for (const auto& s : tmp) out->insert(s);
  return true;
}

static bool is_terminal_step_state(const std::string& s) {
  return s == "SUCCEEDED" || s == "FAILED" || s == "TIMED_OUT" || s == "CANCELED";
}

static int parse_attempt_from_idempotency_key_best_effort(const std::string& idk) {
  // Expect suffix like ":a1" / ":a2" (used by edge workflow retries).
  const std::string needle = ":a";
  const size_t pos = idk.rfind(needle);
  if (pos == std::string::npos) return 0;
  const std::string tail = idk.substr(pos + needle.size());
  if (tail.empty()) return 0;
  int64_t v = 0;
  try {
    v = std::stoll(tail);
  } catch (...) {
    return 0;
  }
  if (v < 0) return 0;
  if (v > INT32_MAX) v = INT32_MAX;
  return (int)v;
}

static int64_t retry_delay_ms_best_effort(int attempt_already_dispatched, int backoff_ms) {
  // attempt_already_dispatched = 1 means first attempt already happened, so first retry uses base backoff.
  int base = backoff_ms;
  if (base <= 0) base = 250;
  if (base > 600000) base = 600000;
  int exp = attempt_already_dispatched - 1;
  if (exp < 0) exp = 0;
  if (exp > 6) exp = 6;
  int64_t mul = (int64_t)1 << exp;
  int64_t out = (int64_t)base * mul;
  if (out < 0) out = base;
  if (out > 3600000) out = 3600000;
  return out;
}

static bool select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  std::string* out_node_id
) {
  if (out_node_id) out_node_id->clear();
  if (!db || !db->is_open() || !out_node_id) return false;

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db->list_edge_nodes(/*max_rows=*/256, &nodes, &err)) return false;

  for (const auto& n : nodes) {
    if (n.node_id.empty()) continue;

    std::unordered_set<std::string> toolset;
    std::unordered_set<std::string> tagset;
    if (!parse_string_set_json(n.tools_json, &toolset)) continue;
    if (!parse_string_set_json(n.tags_json, &tagset)) continue;

    bool ok = true;
    for (const auto& t : requires_tools) {
      if (t.empty()) continue;
      if (!toolset.count(t)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    for (const auto& tag : tags_all) {
      if (tag.empty()) continue;
      if (!tagset.count(tag)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    if (!tags_any.empty()) {
      bool any = false;
      for (const auto& tag : tags_any) {
        if (tag.empty()) continue;
        if (tagset.count(tag)) {
          any = true;
          break;
        }
      }
      if (!any) continue;
    }

    for (const auto& tag : tags_none) {
      if (tag.empty()) continue;
      if (tagset.count(tag)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    *out_node_id = n.node_id;
    return true;
  }
  return false;
}

static bool deps_satisfied_all_succeeded(
  const std::vector<std::string>& deps,
  const std::unordered_map<std::string, AgentDb::EdgeWorkflowStepRow>& step_map
) {
  for (const auto& d : deps) {
    if (d.empty()) continue;
    auto it = step_map.find(d);
    if (it == step_map.end()) return false;
    if (it->second.state != "SUCCEEDED") return false;
  }
  return true;
}

static void compute_workflow_status(
  const std::vector<AgentDb::EdgeWorkflowStepRow>& steps,
  std::string* out_status,
  std::string* out_error
) {
  if (out_status) *out_status = "QUEUED";
  if (out_error) out_error->clear();
  bool any_failed = false;
  bool any_running = false;
  bool all_succeeded = true;
  std::string first_err;
  for (const auto& s : steps) {
    if (s.state == "FAILED" || s.state == "TIMED_OUT" || s.state == "CANCELED") {
      any_failed = true;
      if (first_err.empty() && !s.error.empty()) first_err = s.error;
    }
    if (s.state == "QUEUED" || s.state == "RUNNING") any_running = true;
    if (s.state != "SUCCEEDED") all_succeeded = false;
    if (s.state == "PENDING") all_succeeded = false;
  }
  if (any_failed) {
    if (out_status) *out_status = "FAILED";
    if (out_error) *out_error = first_err;
    return;
  }
  if (all_succeeded) {
    if (out_status) *out_status = "SUCCEEDED";
    return;
  }
  if (any_running) {
    if (out_status) *out_status = "RUNNING";
    return;
  }
  if (out_status) *out_status = "QUEUED";
}

}  // namespace

EdgeWorkflowEngine::EdgeWorkflowEngine(AgentDb* db, std::function<DaemonConfig()> cfg_snapshot, Options opt)
  : db_(db), cfg_snapshot_(std::move(cfg_snapshot)), opt_(opt) {}

EdgeWorkflowEngine::~EdgeWorkflowEngine() {
  stop();
}

bool EdgeWorkflowEngine::start(std::string* out_error) {
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

void EdgeWorkflowEngine::stop() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

void EdgeWorkflowEngine::worker_main() {
  while (!stop_) {
    int poll_ms = opt_.poll_ms;
    if (poll_ms < 1) poll_ms = 1;
    if (cfg_snapshot_) {
      const DaemonConfig cfg = cfg_snapshot_();
      (void)cfg;
    }

    std::vector<AgentDb::EdgeWorkflowRow> queued;
    std::vector<AgentDb::EdgeWorkflowRow> running;
    std::string err;
    if (db_ && db_->is_open()) {
      (void)db_->list_edge_workflows_by_status("QUEUED", opt_.max_scan_workflows, &queued, &err);
      (void)db_->list_edge_workflows_by_status("RUNNING", opt_.max_scan_workflows, &running, &err);
    }
    std::vector<AgentDb::EdgeWorkflowRow> wfs;
    wfs.reserve(queued.size() + running.size());
    for (auto& w : queued) wfs.push_back(std::move(w));
    for (auto& w : running) wfs.push_back(std::move(w));

    size_t dispatch_budget = opt_.max_dispatch_per_tick;

    for (auto& wf : wfs) {
      if (stop_) break;
      if (!db_ || !db_->is_open()) break;
      if (dispatch_budget == 0) break;
      if (wf.workflow_id.empty()) continue;

      // Cancellation/terminal guard:
      // A workflow may be cancelled while we are mid-tick (after we listed queued/running).
      // Always refresh the current status from DB and treat terminal states as authoritative.
      {
        AgentDb::EdgeWorkflowRow live;
        std::string werr;
        if (db_->get_edge_workflow(wf.workflow_id, &live, &werr) && !live.workflow_id.empty()) {
          wf = std::move(live);
        }
        if (wf.status == "CANCELED" || wf.status == "SUCCEEDED" || wf.status == "FAILED") {
          continue;
        }
      }

      std::vector<AgentDb::EdgeWorkflowStepRow> steps;
      std::string serr;
      if (!db_->list_edge_workflow_steps(wf.workflow_id, &steps, &serr)) continue;

      // Cancellation guard (again): cancel can race between the workflow status refresh and step listing.
      {
        AgentDb::EdgeWorkflowRow live;
        std::string werr;
        if (db_->get_edge_workflow(wf.workflow_id, &live, &werr) && live.status == "CANCELED") {
          continue;
        }
      }

      std::unordered_map<std::string, AgentDb::EdgeWorkflowStepRow> step_map;
      step_map.reserve(steps.size());
      for (const auto& s : steps) step_map[s.step_id] = s;

      const int64_t now = unix_ms_now();

      // Recover step state from existing edge_tasks (durability after restart).
      for (auto& s : steps) {
        if (s.kind == "join") continue;
        if (s.state != "PENDING") continue;
        AgentDb::EdgeTaskRow tr;
        std::string terr;
        if (db_->get_edge_task(wf.workflow_id, s.step_id, &tr, &terr) && !tr.state.empty()) {
          s.state = tr.state;
          if (s.attempt <= 0 && !tr.idempotency_key.empty()) {
            const int a = parse_attempt_from_idempotency_key_best_effort(tr.idempotency_key);
            if (a > 0) s.attempt = a;
          }
          s.updated_utc_ms = now;
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
        }
      }

      // Retry scheduling: FAILED/TIMED_OUT may become PENDING again if attempts remain and before deadline.
      for (auto& s : steps) {
        if (stop_) break;
        if (s.kind == "join") continue;
        if (s.state != "FAILED" && s.state != "TIMED_OUT") continue;
        if (s.max_attempts < 1) s.max_attempts = 1;
        if (s.attempt < 0) s.attempt = 0;
        if (s.attempt >= s.max_attempts) continue;
        if (s.deadline_utc_ms > 0 && now >= s.deadline_utc_ms) continue;

        const int64_t delay = retry_delay_ms_best_effort(s.attempt, s.backoff_ms);
        const int64_t next_ready = now + delay;
        if (s.next_ready_utc_ms <= 0 || next_ready > s.next_ready_utc_ms) {
          s.next_ready_utc_ms = next_ready;
        }
        s.state = "PENDING";
        s.updated_utc_ms = now;
        (void)db_->upsert_edge_workflow_step(s, nullptr);
        step_map[s.step_id] = s;

        AgentDb::EdgeWorkflowEventRow ev;
        ev.workflow_id = wf.workflow_id;
        ev.ts_utc_ms = now;
        ev.type = "step_retry_scheduled";
        Json::Value d(Json::objectValue);
        d["workflow_id"] = wf.workflow_id;
        d["step_id"] = s.step_id;
        d["attempt_next"] = s.attempt + 1;
        d["ready_utc_ms"] = (Json::Int64)s.next_ready_utc_ms;
        ev.data_json = json_stringify_compact(d);
        (void)db_->insert_edge_workflow_event(ev, nullptr, nullptr);
      }

      // Evaluate join steps (platform-only).
      for (auto& s : steps) {
        if (stop_) break;
        if (s.kind != "join") continue;
        if (is_terminal_step_state(s.state)) continue;
        std::vector<std::string> deps;
        if (!parse_string_array_json(s.depends_on_json, &deps)) continue;
        const std::string join_mode = s.join_mode.empty() ? "all" : s.join_mode;
        bool any_succ = false;
        bool any_fail = false;
        bool all_term = true;
        bool all_succ = true;
        for (const auto& d : deps) {
          if (d.empty()) continue;
          auto it = step_map.find(d);
          if (it == step_map.end()) {
            all_term = false;
            all_succ = false;
            continue;
          }
          const auto& ds = it->second.state;
          if (ds == "SUCCEEDED") any_succ = true;
          if (ds == "FAILED" || ds == "TIMED_OUT" || ds == "CANCELED") any_fail = true;
          if (!is_terminal_step_state(ds)) all_term = false;
          if (ds != "SUCCEEDED") all_succ = false;
        }
        std::string next_state = s.state;
        std::string next_err = s.error;
        if (join_mode == "any") {
          if (any_succ) next_state = "SUCCEEDED";
          else if (all_term && !any_succ) {
            next_state = "FAILED";
            if (next_err.empty()) next_err = "join(any) failed: no dependency succeeded";
          }
        } else { // all
          if (any_fail) {
            next_state = "FAILED";
            if (next_err.empty()) next_err = "join(all) failed: dependency failed";
          } else if (all_succ) {
            next_state = "SUCCEEDED";
          }
        }
        if (next_state != s.state) {
          s.state = next_state;
          s.error = next_err;
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
        }
      }

      // Dispatch runnable steps.
      for (auto& s : steps) {
        if (stop_) break;
        if (dispatch_budget == 0) break;
        if (s.kind == "join") continue;
        if (s.state != "PENDING") continue;

        // Cancellation guard: check again before dispatching any step to avoid overriding a fresh cancel.
        {
          AgentDb::EdgeWorkflowRow live;
          std::string werr;
          if (db_->get_edge_workflow(wf.workflow_id, &live, &werr) && live.status == "CANCELED") {
            wf = std::move(live);
            break;
          }
        }

        if (s.max_attempts < 1) s.max_attempts = 1;
        if (s.attempt < 0) s.attempt = 0;
        if (s.attempt >= s.max_attempts) {
          s.state = "FAILED";
          if (s.error.empty()) s.error = "max_attempts exceeded";
          s.updated_utc_ms = now;
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }
        if (s.next_ready_utc_ms > 0 && now < s.next_ready_utc_ms) continue;

        std::vector<std::string> deps;
        if (!parse_string_array_json(s.depends_on_json, &deps)) continue;
        if (!deps_satisfied_all_succeeded(deps, step_map)) continue;

        Json::Value target;
        std::string perr;
        if (!json_parse_any(s.target_json, &target, &perr) || !target.isObject()) {
          s.state = "FAILED";
          s.error = "invalid target_json";
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        std::string node_id;
        if (target.isMember("node_id") && target["node_id"].isString()) {
          node_id = trim_copy(target["node_id"].asString());
        } else if (target.isMember("match_any") && target["match_any"].isObject()) {
          const auto& m = target["match_any"];
          std::vector<std::string> requires_tools, tags_all, tags_any, tags_none;
          auto read_arr = [&](const char* k, std::vector<std::string>* out) {
            if (!out) return;
            out->clear();
            if (!m.isMember(k) || !m[k].isArray()) return;
            for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
              if (!m[k][i].isString()) continue;
              out->push_back(m[k][i].asString());
            }
          };
          read_arr("requires_tools", &requires_tools);
          read_arr("tags_all", &tags_all);
          read_arr("tags_any", &tags_any);
          read_arr("tags_none", &tags_none);
          (void)select_node_match_any(db_, requires_tools, tags_all, tags_any, tags_none, &node_id);
        }
        if (node_id.empty() || !id_is_safe(node_id)) {
          s.state = "FAILED";
          s.error = "no matching node";
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        Json::Value payload;
        if (!json_parse_any(s.payload_json, &payload, &perr) || !payload.isObject()) {
          s.state = "FAILED";
          s.error = "invalid payload_json";
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        const std::string mode = (s.kind == "invoke_tool") ? "invoke" : (s.kind == "run_agent") ? "agent" : "";
        if (mode.empty()) {
          s.state = "FAILED";
          s.error = "unsupported step kind";
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        int64_t deadline_utc_ms = s.deadline_utc_ms;
        if (deadline_utc_ms <= 0) deadline_utc_ms = unix_ms_now() + 60000;

        const int attempt_next = s.attempt + 1;
        const std::string idempotency_key =
          std::string("wf:") + wf.workflow_id + ":" + s.step_id + ":a" + std::to_string(attempt_next);

        int64_t outbox_id = 0;
        bool deduped = false;
        std::string derr;
        int http = 500;
        const std::unordered_set<std::string> allow_hazards;
        const bool allow_high_side_effect = false;
        const bool ok = edge_enqueue_task_assign(
          db_,
          node_id,
          wf.workflow_id,
          s.step_id,
          idempotency_key,
          mode,
          deadline_utc_ms,
          attempt_next,
          payload,
          allow_hazards,
          allow_high_side_effect,
          /*enforce_safety=*/true,
          /*enforce_rate_limit=*/true,
          &outbox_id,
          &deduped,
          &derr,
          &http
        );
        if (!ok) {
          if (http == 429) {
            s.next_ready_utc_ms = now + 1000;
            s.updated_utc_ms = now;
            if (s.error.empty()) s.error = derr.empty() ? "rate_limited" : derr;
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          // Transient dispatch failures are treated as retryable without consuming an attempt.
          if (http >= 500) {
            s.next_ready_utc_ms = now + 1000;
            s.updated_utc_ms = now;
            if (s.error.empty()) s.error = derr.empty() ? "dispatch_failed" : derr;
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          s.state = "FAILED";
          s.error = derr.empty() ? "dispatch denied" : derr;
          s.updated_utc_ms = now;
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        s.state = "QUEUED";
        s.attempt = attempt_next;
        s.next_ready_utc_ms = 0;
        s.updated_utc_ms = now;
        (void)db_->upsert_edge_workflow_step(s, nullptr);
        step_map[s.step_id] = s;

        {
          AgentDb::EdgeWorkflowEventRow ev;
          ev.workflow_id = wf.workflow_id;
          ev.ts_utc_ms = now;
          ev.type = "step_dispatched";
          Json::Value d(Json::objectValue);
          d["workflow_id"] = wf.workflow_id;
          d["step_id"] = s.step_id;
          d["node_id"] = node_id;
          d["attempt"] = attempt_next;
          d["idempotency_key"] = idempotency_key;
          d["outbox_id"] = (Json::Int64)outbox_id;
          if (deduped) d["deduped"] = true;
          ev.data_json = json_stringify_compact(d);
          (void)db_->insert_edge_workflow_event(ev, nullptr, nullptr);
        }

        wf.status = "RUNNING";
        wf.updated_utc_ms = now;
        (void)db_->upsert_edge_workflow(wf, nullptr);

        dispatch_budget--;
      }

      // Update workflow status based on current steps.
      {
        AgentDb::EdgeWorkflowRow live;
        std::string werr;
        if (db_->get_edge_workflow(wf.workflow_id, &live, &werr) && live.status == "CANCELED") {
          // Cancellation is authoritative; do not override with derived status.
          continue;
        }
      }
      std::string next_status;
      std::string next_err;
      compute_workflow_status(steps, &next_status, &next_err);
      if (!next_status.empty() && (wf.status != next_status || (next_status == "FAILED" && wf.error != next_err))) {
        wf.status = next_status;
        wf.updated_utc_ms = now;
        if (next_status == "FAILED" && !next_err.empty()) wf.error = next_err;
        (void)db_->upsert_edge_workflow(wf, nullptr);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
}

}  // namespace agentd
