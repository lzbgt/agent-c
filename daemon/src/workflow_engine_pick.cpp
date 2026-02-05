#include "workflow_engine.h"

#include "json_util.h"
#include "string_util.h"
#include "workflow_engine_common.h"
#include "workflow_fairq_cost.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace agentd {
namespace wfi = workflow_engine_internal;
using namespace wfi;

bool WorkflowEngine::pick_and_claim_one(
  int64_t now_unix_ms,
  AgentDb::WorkflowRow* out_wf,
  AgentDb::WorkflowTaskRow* out_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_wf || !out_task) return false;
  *out_wf = AgentDb::WorkflowRow{};
  *out_task = AgentDb::WorkflowTaskRow{};

  // Scheduling fairness note:
  //
  // `list_workflows_by_status(..., LIMIT N)` can accidentally starve older workflows when many newer ones exist,
  // because the older workflow may never appear in the top-N results. To mitigate this, we intentionally
  // oversample the DB query and then apply an in-memory scheduling policy over the larger candidate set.
  //
  // This stays bounded by the DB helper's internal clamp (<=512 rows per query).
  const size_t fetch_max = std::max<size_t>(16, opt_.max_scan_workflows) * 8;

  std::vector<AgentDb::WorkflowRow> queued;
  std::vector<AgentDb::WorkflowRow> running;
  std::string err;
  if (!db_->list_workflows_by_status_for_scheduler("queued", fetch_max, &queued, &err)) {
    if (out_error) *out_error = err;
    return false;
  }
  if (!db_->list_workflows_by_status_for_scheduler("running", fetch_max, &running, &err)) {
    if (out_error) *out_error = err;
    return false;
  }

  std::vector<AgentDb::WorkflowRow> all;
  all.reserve(queued.size() + running.size());
  all.insert(all.end(), queued.begin(), queued.end());
  all.insert(all.end(), running.begin(), running.end());

  // De-dup by workflow_id, then apply scheduling order.
  std::unordered_set<std::string> seen;
  std::vector<AgentDb::WorkflowRow> wfs;
  wfs.reserve(all.size());
  for (auto& wf : all) {
    if (wf.workflow_id.empty()) continue;
    if (seen.insert(wf.workflow_id).second) wfs.push_back(std::move(wf));
  }

  auto wf_prio = [](const AgentDb::WorkflowRow& w) -> int {
    return (w.priority == AgentDb::kIntUnset) ? 0 : w.priority;
  };
  auto wf_status_rank = [](const std::string& s) -> int {
    if (s == "running") return 2;
    if (s == "queued") return 1;
    return 0;
  };
  std::sort(wfs.begin(), wfs.end(), [&](const AgentDb::WorkflowRow& a, const AgentDb::WorkflowRow& b) {
    const int ap = wf_prio(a);
    const int bp = wf_prio(b);
    if (ap != bp) return ap > bp;
    const int ar = wf_status_rank(a.status);
    const int br = wf_status_rank(b.status);
    if (ar != br) return ar > br;
    // Fairness: for the same priority/status, prefer older workflows first (avoid starvation).
    // Use created_unix_ms (stable age) rather than updated_unix_ms (changes during execution).
    if (a.created_unix_ms != b.created_unix_ms) return a.created_unix_ms < b.created_unix_ms;
    return a.workflow_id < b.workflow_id;
  });

  auto session_bucket_key = [](const AgentDb::WorkflowRow& wf) -> std::string {
    if (!wf.session_id.empty()) return std::string("sid:") + wf.session_id;
    // Treat session-less workflows as independent buckets so a "no_session" client can't monopolize
    // the scan order by submitting many workflows without a session_id.
    return std::string("wf:") + wf.workflow_id;
  };

  // Session-aware scan order:
  // - sessions are ordered by the best workflow in that session under the base ordering (priority/status/age).
  // - within a session, workflows keep their original order.
  // - a rr cursor selects the session start point to avoid thundering herd.
  std::unordered_map<std::string, std::vector<size_t>> wf_idxs_by_session;
  std::vector<std::string> sessions;
  sessions.reserve(wfs.size());
  wf_idxs_by_session.reserve(wfs.size());

  for (size_t i = 0; i < wfs.size(); i++) {
    const std::string k = session_bucket_key(wfs[i]);
    auto& vec = wf_idxs_by_session[k];
    if (vec.empty()) sessions.push_back(k);
    vec.push_back(i);
  }

  const size_t ns = sessions.size();
  if (ns == 0) return false;
  const uint64_t cursor = rr_cursor_.fetch_add(1);

  auto try_pick_in_workflow = [&](AgentDb::WorkflowRow& wf) -> bool {
    if (stop_.load()) return false;
    if (wf.workflow_id.empty()) return false;
    if (wf.status != "queued" && wf.status != "running") return false;

    // Scheduler-level deadline guard (best-effort): if the submit spec carries a deadline and it has passed,
    // stop admitting new tasks for this workflow and cancel queued tasks. Running tasks are not forcibly interrupted.
    const int64_t deadline_unix_ms = workflow_deadline_unix_ms_best_effort(wf);
    if (deadline_unix_ms > 0 && now_unix_ms > deadline_unix_ms) {
      bool any_change = false;
      std::vector<AgentDb::WorkflowTaskRow> tasks;
      if (db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
        for (auto& t : tasks) {
          if (t.status == "queued") {
            t.status = "cancelled";
            t.updated_unix_ms = now_unix_ms;
            t.finished_unix_ms = now_unix_ms;
            t.error = "deadline exceeded";
            (void)db_->upsert_workflow_task(t, nullptr);
            {
              Json::Value d(Json::objectValue);
              d["workflow_id"] = wf.workflow_id;
              d["task_id"] = t.task_id;
              d["status"] = t.status;
              d["attempt"] = t.attempt;
              d["max_attempts"] = t.max_attempts;
              d["reason"] = "deadline_exceeded";
              d["error"] = t.error;
              d["ts_unix_ms"] = (Json::Int64)now_unix_ms;
              insert_workflow_event_best_effort(db_, wf.workflow_id, t.task_id, "task_status", now_unix_ms, d);
            }
            any_change = true;
          }
        }
      }

      if (!wf.cancel_requested || wf.error != "deadline exceeded") {
        AgentDb::WorkflowRow upd = wf;
        upd.cancel_requested = true;
        upd.updated_unix_ms = now_unix_ms;
        upd.error = "deadline exceeded";
        (void)db_->upsert_workflow(upd, nullptr);
        any_change = true;
      }
      if (any_change) {
        maybe_finalize_workflow(wf.workflow_id);
      }
      return false;
    }

    std::vector<AgentDb::WorkflowTaskRow> tasks;
    if (!db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      return false;
    }

    // Fairness/budgets: skip workflows that are already saturating their in-flight budget.
    if (opt_.max_inflight_per_workflow > 0) {
      int running_cnt = 0;
      for (const auto& t : tasks) {
        if (t.status == "running") running_cnt++;
      }
      if (running_cnt >= opt_.max_inflight_per_workflow) {
        return false;
      }
    }

    // If cancel requested, try to cancel queued tasks best-effort.
    if (wf.cancel_requested) {
      bool any_change = false;
      for (auto& t : tasks) {
        if (t.status == "queued") {
          t.status = "cancelled";
          t.updated_unix_ms = now_unix_ms;
          t.error = t.error.empty() ? "cancelled" : t.error;
          (void)db_->upsert_workflow_task(t, nullptr);
          {
            Json::Value d(Json::objectValue);
            d["workflow_id"] = wf.workflow_id;
            d["task_id"] = t.task_id;
            d["status"] = t.status;
            d["attempt"] = t.attempt;
            d["max_attempts"] = t.max_attempts;
            d["reason"] = "cancel_requested";
            if (!t.error.empty()) d["error"] = t.error;
            d["ts_unix_ms"] = (Json::Int64)now_unix_ms;
            insert_workflow_event_best_effort(db_, wf.workflow_id, t.task_id, "task_status", now_unix_ms, d);
          }
          any_change = true;
        }
      }
      if (any_change) {
        maybe_finalize_workflow(wf.workflow_id);
        // Keep scanning in case another workflow is runnable.
      }
      return false;
    }

    std::unordered_map<std::string, std::string> status_by_id;
    status_by_id.reserve(tasks.size());
    std::unordered_map<std::string, bool> allow_error_by_id;
    allow_error_by_id.reserve(tasks.size());
    for (const auto& t : tasks) {
      status_by_id[t.task_id] = t.status;
      allow_error_by_id[t.task_id] = t.allow_error;
    }

    auto task_prio = [](const AgentDb::WorkflowTaskRow& t) -> int {
      return (t.priority == AgentDb::kIntUnset) ? 0 : t.priority;
    };
    std::stable_sort(tasks.begin(), tasks.end(), [&](const AgentDb::WorkflowTaskRow& a, const AgentDb::WorkflowTaskRow& b) {
      const int ap = task_prio(a);
      const int bp = task_prio(b);
      if (ap != bp) return ap > bp;
      const int64_t ar = a.ready_unix_ms;
      const int64_t br = b.ready_unix_ms;
      if (ar != br) {
        const int64_t ar2 = ar > 0 ? ar : 0;
        const int64_t br2 = br > 0 ? br : 0;
        if (ar2 != br2) return ar2 < br2;
      }
      if (a.created_unix_ms != b.created_unix_ms) return a.created_unix_ms < b.created_unix_ms;
      return a.task_id < b.task_id;
    });

    for (auto& t : tasks) {
      if (t.status != "queued") continue;
      if (t.ready_unix_ms > 0 && now_unix_ms < t.ready_unix_ms) continue;

      const std::string kind = workflow_task_kind_best_effort(t);

      const std::vector<std::string> deps = parse_dep_ids(t.depends_on_json);
      bool deps_ok = true;
      for (const auto& dep : deps) {
        auto it = status_by_id.find(dep);
        if (it == status_by_id.end()) {
          deps_ok = false;
          break;
        }
        const std::string& st = it->second;
        if (st == "done") continue;
        // Allow a dependent to proceed if the dependency is a soft-fail error.
        const bool dep_allow_error =
          allow_error_by_id.count(dep) ? allow_error_by_id[dep] : false;
        if (st == "error" && dep_allow_error) continue;
        // Aggregation tasks often need to read terminal outcomes (including error) to compute a join result.
        if (kind == "aggregate" && workflow_is_terminal_status(st)) continue;
        deps_ok = false;
        break;
      }
      if (!deps_ok) continue;

      const int new_attempt = t.attempt + 1;
      if (!db_->claim_workflow_task_budgeted(
            wf.workflow_id,
            t.task_id,
            now_unix_ms,
            new_attempt,
            opt_.max_inflight_per_workflow,
            opt_.max_inflight_per_session,
            wf.session_id,
            &err)) {
        continue;
      }

      // Mark workflow as running once it has any running task.
      if (wf.status != "running") {
        wf.status = "running";
        wf.updated_unix_ms = now_unix_ms;
        (void)db_->upsert_workflow(wf, nullptr);
        {
          Json::Value d(Json::objectValue);
          d["workflow_id"] = wf.workflow_id;
          d["status"] = wf.status;
          d["ts_unix_ms"] = (Json::Int64)now_unix_ms;
          insert_workflow_event_best_effort(db_, wf.workflow_id, "", "workflow_status", now_unix_ms, d);
        }
      }

      t.status = "running";
      t.attempt = new_attempt;
      t.started_unix_ms = now_unix_ms;
      t.updated_unix_ms = now_unix_ms;
      {
        Json::Value d(Json::objectValue);
        d["workflow_id"] = wf.workflow_id;
        d["task_id"] = t.task_id;
        d["status"] = "running";
        d["attempt"] = t.attempt;
        d["max_attempts"] = t.max_attempts;
        d["started_unix_ms"] = (Json::Int64)t.started_unix_ms;
        d["ts_unix_ms"] = (Json::Int64)now_unix_ms;
        insert_workflow_event_best_effort(db_, wf.workflow_id, t.task_id, "task_status", now_unix_ms, d);
      }
      *out_wf = wf;
      *out_task = t;
      return true;
    }

    // No runnable tasks; check if the workflow is now terminal.
    maybe_finalize_workflow(wf.workflow_id);
    return false;
  };

	  if (opt_.fair_queue_policy == "drr") {
	    // Deficit Round Robin over session buckets.
	    //
	    // This avoids expanding a potentially large WRR schedule vector, and creates a clean hook for
	    // future cost-aware quanta (v2.3+).
	    //
	    // Current v2.3 semantics (minimal):
	    // - quantum = session_weight (clamped)
	    // - each admitted task costs 1
	    // - deficits are loaded/persisted in the DB (best-effort) so fairness survives daemon restarts
	    const int max_weight_cfg = std::max(1, opt_.fair_queue_max_session_weight);
	    const std::string drr_cost_model = trim_copy(opt_.drr_cost_model.empty() ? "unit" : opt_.drr_cost_model);

	    std::unordered_map<std::string, int> weight_by_session;
	    weight_by_session.reserve(ns);
	    for (size_t si = 0; si < ns; si++) {
	      const std::string& sk = sessions[si];
	      int w = 1;
	      auto it = wf_idxs_by_session.find(sk);
	      if (it != wf_idxs_by_session.end()) {
	        const auto& idxs = it->second;
	        for (size_t wi = 0; wi < idxs.size(); wi++) {
	          const size_t idx = idxs[wi];
	          if (idx >= wfs.size()) continue;
	          w = std::max(w, workflow_session_weight_best_effort(wfs[idx], max_weight_cfg));
	          if (w >= max_weight_cfg) break;
	        }
	      }
	      weight_by_session[sk] = std::max(1, std::min(max_weight_cfg, w));
	    }

	    auto persist_session_id_from_bucket = [](const std::string& bucket_key) -> std::string {
	      const std::string prefix = "sid:";
	      if (bucket_key.size() > prefix.size() && bucket_key.rfind(prefix, 0) == 0) {
	        return bucket_key.substr(prefix.size());
	      }
	      return "";
	    };

	    // Best-effort: load persisted deficits for any session we haven't seen in this process.
	    // This is done outside the fairq mutex to avoid blocking other workers on sqlite I/O.
	    std::vector<std::pair<std::string, std::string>> to_load; // (bucket_key, session_id)
	    to_load.reserve(ns);
	    {
	      std::lock_guard<std::mutex> lk(fairq_mu_);
	      for (const auto& sk : sessions) {
	        const std::string sid = persist_session_id_from_bucket(sk);
	        if (sid.empty()) continue; // session-less buckets are not persisted
	        if (drr_loaded_sessions_.count(sid)) continue;
	        drr_loaded_sessions_.insert(sid);
	        to_load.push_back({sk, sid});
	      }
	    }
	    for (const auto& it : to_load) {
	      const std::string& sk = it.first;
	      const std::string& sid = it.second;
	      AgentDb::WorkflowFairqSessionRow r;
	      std::string lerr;
	      const bool found = db_->get_workflow_fairq_session(sid, &r, &lerr);
	      if (!found) continue;
	      // Allow negative deficits (debt) for future cost-aware scheduling, but clamp to avoid pathological values.
	      const int64_t d0 = std::max<int64_t>(-1000000, std::min<int64_t>(1000000, r.deficit));
	      {
	        std::lock_guard<std::mutex> lk(fairq_mu_);
	        drr_deficit_by_session_[sk] = d0;
	      }
	    }

	    std::vector<std::string> attempt_sessions;
	    attempt_sessions.reserve(ns);
	    {
	      std::lock_guard<std::mutex> lk(fairq_mu_);

	      // Keep the deficit map bounded: if it grows unusually large, prune to the active set.
	      if (drr_deficit_by_session_.size() > 2048) {
	        std::unordered_set<std::string> active;
	        active.reserve(ns);
	        for (const auto& sk : sessions) active.insert(sk);
	        for (auto it = drr_deficit_by_session_.begin(); it != drr_deficit_by_session_.end();) {
	          if (active.count(it->first)) ++it;
	          else {
	            const std::string sid = persist_session_id_from_bucket(it->first);
	            if (!sid.empty()) drr_loaded_sessions_.erase(sid);
	            it = drr_deficit_by_session_.erase(it);
	          }
	        }
	      }

	      // Add quantum to all active sessions (saturating).
	      for (const auto& sk : sessions) {
	        const int q = weight_by_session.count(sk) ? weight_by_session[sk] : 1;
	        int64_t& d = drr_deficit_by_session_[sk];
	        if (q > 0) {
	          if (d > (INT64_MAX - (int64_t)q)) d = INT64_MAX;
	          else d += (int64_t)q;
	        }
	      }

	      const size_t start = (size_t)(cursor % (uint64_t)ns);
	      for (size_t si = 0; si < ns; si++) {
	        attempt_sessions.push_back(sessions[(start + si) % ns]);
	      }
	    }

	    auto get_deficit = [&](const std::string& sk) -> int64_t {
	      std::lock_guard<std::mutex> lk(fairq_mu_);
	      auto it = drr_deficit_by_session_.find(sk);
	      return (it == drr_deficit_by_session_.end()) ? 0 : it->second;
	    };
	    auto charge_deficit = [&](const std::string& sk, int64_t cost) {
	      if (cost <= 0) return;
	      int64_t new_def = 0;
	      std::lock_guard<std::mutex> lk(fairq_mu_);
	      auto it = drr_deficit_by_session_.find(sk);
	      if (it == drr_deficit_by_session_.end()) return;
	      it->second = it->second - cost;
	      // Keep the in-memory map bounded and stable.
	      if (it->second > 1000000) it->second = 1000000;
	      if (it->second < -1000000) it->second = -1000000;
	      new_def = it->second;
	      (void)new_def;
	    };

	    for (size_t ssi = 0; ssi < attempt_sessions.size(); ssi++) {
	      const std::string& sk = attempt_sessions[ssi];
	      if (get_deficit(sk) < 1) continue;
	      auto it = wf_idxs_by_session.find(sk);
	      if (it == wf_idxs_by_session.end()) continue;
	      const auto& idxs = it->second;
	      for (size_t wi = 0; wi < idxs.size(); wi++) {
	        const size_t idx = idxs[wi];
	        if (idx >= wfs.size()) continue;
	        if (try_pick_in_workflow(wfs[idx])) {
	          int64_t cost = 1;
	          if (drr_cost_model == "simple_v1") {
	            cost = workflow_fairq_estimate_task_cost_simple_v1(out_task->request_json, /*max_cost=*/32);
	          }
	          charge_deficit(sk, std::max<int64_t>(1, cost));
	          // Persist the updated deficit (best-effort). Failures should not block scheduling.
	          const std::string sid = persist_session_id_from_bucket(sk);
	          if (!sid.empty()) {
	            AgentDb::WorkflowFairqSessionRow rr;
	            rr.session_id = sid;
	            rr.deficit = get_deficit(sk);
	            rr.weight = weight_by_session.count(sk) ? weight_by_session[sk] : 1;
	            rr.updated_unix_ms = now_unix_ms;
	            (void)db_->upsert_workflow_fairq_session(rr, nullptr);
	          }
	          return true;
	        }
	      }
	    }
	  } else if (opt_.fair_queue_policy == "wrr") {
	    // Weighted round-robin over session buckets.
	    // The base `sessions` order is derived from (priority DESC, status, age) of the best workflow in each bucket.
	    //
	    // The weight is extracted from workflow submit spec's `session_weight` (best-effort), but only when the
	    // workflow is session-scoped (wf.session_id is not empty).
    //
    // Guardrail: bound the expanded schedule length to keep the scheduler overhead stable.
    const int max_weight_cfg = std::max(1, opt_.fair_queue_max_session_weight);
    const size_t max_len_cfg = (size_t)std::max(16, opt_.fair_queue_max_schedule_len);
    int max_weight_eff = max_weight_cfg;
    if (ns > 0) {
      const size_t cap_per = std::max<size_t>(1, max_len_cfg / ns);
      max_weight_eff = std::min<int>(max_weight_eff, (int)cap_per);
    }

    std::vector<std::string> schedule;
    schedule.reserve(std::min<size_t>(max_len_cfg, ns * (size_t)std::max(1, max_weight_eff)));
    for (size_t si = 0; si < ns; si++) {
      const std::string& sk = sessions[si];
      int w = 1;
      auto it = wf_idxs_by_session.find(sk);
      if (it != wf_idxs_by_session.end()) {
        const auto& idxs = it->second;
        for (size_t wi = 0; wi < idxs.size(); wi++) {
          const size_t idx = idxs[wi];
          if (idx >= wfs.size()) continue;
          w = std::max(w, workflow_session_weight_best_effort(wfs[idx], max_weight_eff));
          if (w >= max_weight_eff) break;
        }
      }
      for (int j = 0; j < w; j++) {
        if (schedule.size() >= max_len_cfg) break;
        schedule.push_back(sk);
      }
      if (schedule.size() >= max_len_cfg) break;
    }

    const size_t nsch = schedule.size();
    if (nsch > 0) {
      const size_t start = (size_t)(cursor % (uint64_t)nsch);
      for (size_t ssi = 0; ssi < nsch; ssi++) {
        const std::string& sk = schedule[(start + ssi) % nsch];
        auto it = wf_idxs_by_session.find(sk);
        if (it == wf_idxs_by_session.end()) continue;
        const auto& idxs = it->second;
        for (size_t wi = 0; wi < idxs.size(); wi++) {
          const size_t idx = idxs[wi];
          if (idx >= wfs.size()) continue;
          if (try_pick_in_workflow(wfs[idx])) return true;
        }
      }
    }
  } else {
    // Legacy session-aware scan order (round-robin start point over buckets).
    const size_t session_start = (size_t)(cursor % (uint64_t)ns);
    for (size_t si = 0; si < ns; si++) {
      const std::string& sk = sessions[(session_start + si) % ns];
      auto it = wf_idxs_by_session.find(sk);
      if (it == wf_idxs_by_session.end()) continue;
      const auto& idxs = it->second;
      for (size_t wi = 0; wi < idxs.size(); wi++) {
        const size_t idx = idxs[wi];
        if (idx >= wfs.size()) continue;
        if (try_pick_in_workflow(wfs[idx])) return true;
      }
    }
  }

  return false;
}

}  // namespace agentd

