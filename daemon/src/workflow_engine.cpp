#include "workflow_engine.h"

#include "avm_endpoints.h"
#include "edge_util.h"
#include "json_util.h"
#include "memory_consolidator.h"
#include "run_endpoints.h"
#include "string_util.h"
#include "toolset_host.h"
#include "workflow_aggregate.h"
#include "workflow_templates.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static bool json_parse_any_value(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = Json::Value(Json::nullValue);
  std::string perr;
  if (!json_parse_any(s, out, &perr)) {
    if (out_err) *out_err = perr;
    return false;
  }
  return true;
}

static std::vector<std::string> parse_dep_ids(const std::string& deps_json) {
  std::vector<std::string> out;
  if (deps_json.empty()) return out;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(deps_json, &v, &err) || !v.isArray()) return out;
  out.reserve(v.size());
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (v[i].isString()) out.push_back(v[i].asString());
  }
  return out;
}

static std::string json_get_string(const Json::Value& obj, const char* k) {
  if (!k) return "";
  if (!obj.isObject()) return "";
  const auto& v = obj[k];
  return v.isString() ? v.asString() : "";
}

// Very small ${task.<id>.assistant_text} expander for prompts.
static std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool workflow_is_terminal_status(const std::string& s) {
  return (s == "done" || s == "error" || s == "cancelled");
}

static std::string workflow_task_kind_best_effort(const AgentDb::WorkflowTaskRow& t) {
  if (t.request_json.empty()) return "";
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(t.request_json, &v, &err) || !v.isObject()) return "";
  return json_get_string(v, "kind");
}

static int64_t workflow_deadline_unix_ms_best_effort(const AgentDb::WorkflowRow& wf) {
  if (wf.deadline_unix_ms > 0) return wf.deadline_unix_ms;
  if (wf.spec_json.empty()) return 0;
  if (wf.spec_json.find("deadline_unix_ms") == std::string::npos) return 0;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(wf.spec_json, &v, &err) || !v.isObject()) return 0;
  if (!v.isMember("deadline_unix_ms")) return 0;
  const Json::Value& d = v["deadline_unix_ms"];
  if (!(d.isInt64() || d.isUInt64() || d.isInt() || d.isUInt())) return 0;
  const int64_t ms = d.asInt64();
  return ms > 0 ? ms : 0;
}

enum class WorkflowCancelReason {
  None = 0,
  CancelRequested = 1,
  DeadlineExceeded = 2,
};

struct WorkflowRunCancelCtx {
  AgentDb* db = nullptr;
  std::string workflow_id;
  int64_t deadline_unix_ms = 0;
  int64_t last_check_unix_ms = 0;
  WorkflowCancelReason reason = WorkflowCancelReason::None;
};

static bool workflow_run_should_cancel(void* vctx) {
  auto* c = static_cast<WorkflowRunCancelCtx*>(vctx);
  if (!c) return false;

  if (c->reason != WorkflowCancelReason::None) return true;

  const int64_t now = unix_ms_now();
  if (c->deadline_unix_ms > 0 && now > c->deadline_unix_ms) {
    c->reason = WorkflowCancelReason::DeadlineExceeded;
    return true;
  }

  if (!c->db || c->workflow_id.empty()) return false;
  if (c->last_check_unix_ms > 0 && (now - c->last_check_unix_ms) < 50) {
    return false;
  }
  c->last_check_unix_ms = now;
  AgentDb::WorkflowRow wf;
  std::string err;
  if (!c->db->get_workflow(c->workflow_id, &wf, &err)) return false;
  if (wf.cancel_requested) {
    c->reason = WorkflowCancelReason::CancelRequested;
    return true;
  }
  return false;
}

static void insert_workflow_event_best_effort(
  AgentDb* db,
  const std::string& workflow_id,
  const std::string& task_id,
  const std::string& type,
  int64_t ts_unix_ms,
  const Json::Value& data
) {
  if (!db) return;
  if (workflow_id.empty() || type.empty()) return;
  AgentDb::WorkflowEventRow ev;
  ev.workflow_id = workflow_id;
  ev.task_id = task_id;
  ev.ts_unix_ms = ts_unix_ms > 0 ? ts_unix_ms : unix_ms_now();
  ev.type = type;
  ev.data_json = json_stringify_compact_local(data.isNull() ? Json::Value(Json::objectValue) : data);
  (void)db->insert_workflow_event(ev, nullptr, nullptr);
}

static bool apply_expectations(const std::string& expect_json, const Json::Value& run_out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (expect_json.empty()) return true;
  Json::Value exp;
  std::string perr;
  if (!json_parse_any_value(expect_json, &exp, &perr) || !exp.isObject()) {
    if (out_err) *out_err = "invalid expect_json: " + perr;
    return false;
  }

  // Gather tool usage from run events (tool_call + tool_result are emitted by the core tool loop).
  // This is used for deterministic "correctness constraints" like "must call X" or "must not call Y".
  std::unordered_map<std::string, int64_t> tool_call_count_by_name;
  int64_t tool_calls_total = 0;
  if (run_out.isObject() && run_out.isMember("events") && run_out["events"].isArray()) {
    const auto& events = run_out["events"];
    for (Json::ArrayIndex i = 0; i < events.size(); i++) {
      const auto& ev = events[i];
      if (!ev.isObject()) continue;
      const std::string type = ev.isMember("type") && ev["type"].isString() ? ev["type"].asString() : "";
      if (type != "tool_call" && type != "tool_result") continue;
      const auto& data = ev.isMember("data") && ev["data"].isObject() ? ev["data"] : Json::Value(Json::nullValue);
      const std::string tool =
        data.isObject() && data.isMember("tool_name") && data["tool_name"].isString() ? data["tool_name"].asString()
        : "";
      if (tool.empty()) continue;
      if (type == "tool_call") {
        tool_calls_total++;
        tool_call_count_by_name[tool] += 1;
      }
    }
  }

  if (exp.isMember("ok") && exp["ok"].isBool()) {
    const bool want_ok = exp["ok"].asBool();
    const bool got_ok = run_out.isObject() && run_out.isMember("ok") && run_out["ok"].isBool() && run_out["ok"].asBool();
    if (want_ok != got_ok) {
      if (out_err) *out_err = std::string("expectation failed: ok=") + (want_ok ? "true" : "false");
      return false;
    }
  }

  const std::string assistant_text = json_get_string(run_out, "assistant_text");
  if (exp.isMember("assistant_text_contains")) {
    const auto& v = exp["assistant_text_contains"];
    if (v.isString()) {
      const std::string needle = v.asString();
      if (!needle.empty() && assistant_text.find(needle) == std::string::npos) {
        if (out_err) *out_err = "expectation failed: assistant_text does not contain required substring";
        return false;
      }
    } else if (v.isArray()) {
      for (Json::ArrayIndex i = 0; i < v.size(); i++) {
        if (!v[i].isString()) continue;
        const std::string needle = v[i].asString();
        if (!needle.empty() && assistant_text.find(needle) == std::string::npos) {
          if (out_err) *out_err = "expectation failed: assistant_text does not contain required substring";
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_equals")) {
    const auto& v = exp["json_pointer_equals"];
    Json::Value arr = v;
    if (v.isObject()) {
      arr = Json::Value(Json::arrayValue);
      arr.append(v);
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& row = arr[i];
        if (!row.isObject()) continue;
        const std::string ptr = row.isMember("pointer") && row["pointer"].isString() ? row["pointer"].asString() : "";
        if (ptr.empty()) continue;
        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json_pointer missing: " + ptr;
          return false;
        }
        if (!row.isMember("value")) {
          if (out_err) *out_err = "expectation failed: json_pointer_equals missing value";
          return false;
        }
        if (*got != row["value"]) {
          if (out_err) *out_err = "expectation failed: json_pointer_equals mismatch at " + ptr;
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_exists")) {
    Json::Value arr = exp["json_pointer_exists"];
    if (arr.isString()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        if (!arr[i].isString()) continue;
        const std::string ptr = arr[i].asString();
        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing";
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_regex")) {
    Json::Value arr = exp["json_pointer_regex"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string ptr =
          item.isMember("pointer") && item["pointer"].isString() ? item["pointer"].asString() : "";
        const std::string pat =
          item.isMember("regex") && item["regex"].isString() ? item["regex"].asString() : "";
        if (ptr.empty() || pat.empty()) continue;

        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing for regex";
          return false;
        }

        std::string s;
        if (got->isString()) s = got->asString();
        else s = json_stringify_compact_local(*got);

        try {
          const std::regex re(pat);
          if (!std::regex_search(s, re)) {
            if (out_err) *out_err = "expectation failed: regex did not match";
            return false;
          }
        } catch (const std::exception& e) {
          if (out_err) *out_err = std::string("invalid regex: ") + e.what();
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_number_between")) {
    Json::Value arr = exp["json_pointer_number_between"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string ptr =
          item.isMember("pointer") && item["pointer"].isString() ? item["pointer"].asString() : "";
        if (ptr.empty()) continue;

        bool has_min = false;
        bool has_max = false;
        double min_v = 0.0;
        double max_v = 0.0;
        if (item.isMember("min")) has_min = json_value_to_double_best_effort(item["min"], &min_v);
        if (item.isMember("max")) has_max = json_value_to_double_best_effort(item["max"], &max_v);

        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing for numeric bound";
          return false;
        }
        double x = 0.0;
        if (!json_value_to_double_best_effort(*got, &x)) {
          if (out_err) *out_err = "expectation failed: value is not numeric";
          return false;
        }
        if (has_min && x < min_v) {
          if (out_err) *out_err = "expectation failed: value below min";
          return false;
        }
        if (has_max && x > max_v) {
          if (out_err) *out_err = "expectation failed: value above max";
          return false;
        }
      }
    }
  }

  auto normalize_string_or_array = [](const Json::Value& v) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (v.isString()) {
      const std::string s = trim_copy(v.asString());
      if (!s.empty()) out.push_back(s);
    } else if (v.isArray()) {
      for (Json::ArrayIndex i = 0; i < v.size(); i++) {
        if (!v[i].isString()) continue;
        const std::string s = trim_copy(v[i].asString());
        if (!s.empty()) out.push_back(s);
      }
    }
    return out;
  };

  if (exp.isMember("tool_called")) {
    const auto want = normalize_string_or_array(exp["tool_called"]);
    for (const auto& tn : want) {
      if (tool_call_count_by_name[tn] <= 0) {
        if (out_err) *out_err = "expectation failed: required tool was not called: " + tn;
        return false;
      }
    }
  }

  if (exp.isMember("tool_not_called")) {
    const auto forbid = normalize_string_or_array(exp["tool_not_called"]);
    for (const auto& tn : forbid) {
      if (tool_call_count_by_name[tn] > 0) {
        if (out_err) *out_err = "expectation failed: forbidden tool was called: " + tn;
        return false;
      }
    }
  }

  if (exp.isMember("tool_calls_total_between") && exp["tool_calls_total_between"].isObject()) {
    const auto& v = exp["tool_calls_total_between"];
    bool has_min = false;
    bool has_max = false;
    int64_t min_v = 0;
    int64_t max_v = 0;
    if (v.isMember("min") && (v["min"].isInt64() || v["min"].isUInt64() || v["min"].isInt())) {
      has_min = true;
      min_v = v["min"].asInt64();
    }
    if (v.isMember("max") && (v["max"].isInt64() || v["max"].isUInt64() || v["max"].isInt())) {
      has_max = true;
      max_v = v["max"].asInt64();
    }
    if (has_min && tool_calls_total < min_v) {
      if (out_err) *out_err = "expectation failed: tool_calls_total below min";
      return false;
    }
    if (has_max && tool_calls_total > max_v) {
      if (out_err) *out_err = "expectation failed: tool_calls_total above max";
      return false;
    }
  }

  if (exp.isMember("tool_calls_for_tool_between")) {
    Json::Value arr = exp["tool_calls_for_tool_between"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string tool =
          item.isMember("tool") && item["tool"].isString() ? trim_copy(item["tool"].asString()) : "";
        if (tool.empty()) continue;
        const int64_t cnt = tool_call_count_by_name.count(tool) ? tool_call_count_by_name[tool] : 0;
        bool has_min = false;
        bool has_max = false;
        int64_t min_v = 0;
        int64_t max_v = 0;
        if (item.isMember("min") && (item["min"].isInt64() || item["min"].isUInt64() || item["min"].isInt())) {
          has_min = true;
          min_v = item["min"].asInt64();
        }
        if (item.isMember("max") && (item["max"].isInt64() || item["max"].isUInt64() || item["max"].isInt())) {
          has_max = true;
          max_v = item["max"].asInt64();
        }
        if (has_min && cnt < min_v) {
          if (out_err) *out_err = "expectation failed: tool call count below min for tool: " + tool;
          return false;
        }
        if (has_max && cnt > max_v) {
          if (out_err) *out_err = "expectation failed: tool call count above max for tool: " + tool;
          return false;
        }
      }
    }
  }

  return true;
}

static int64_t retry_backoff_ms(int attempt) {
  if (attempt <= 0) return 0;
  // Quadratic-ish backoff with cap.
  int64_t ms = (int64_t)attempt * (int64_t)attempt * 250;
  if (ms < 250) ms = 250;
  if (ms > 60 * 1000) ms = 60 * 1000;
  return ms;
}

}  // namespace

WorkflowEngine::WorkflowEngine(
  AgentDb* db,
  std::function<DaemonConfig()> cfg_snapshot,
  std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg,
  const ToolExtension* tool_ext_or_null,
  std::string sessions_root_dir,
  Options opt
) : db_(db),
    cfg_snapshot_(std::move(cfg_snapshot)),
    ocfg_from_cfg_(std::move(ocfg_from_cfg)),
    tool_ext_or_null_(tool_ext_or_null),
    sessions_root_dir_(std::move(sessions_root_dir)),
    opt_(opt) {
  if (opt_.max_concurrency <= 0) opt_.max_concurrency = 1;
  if (opt_.max_concurrency > 16) opt_.max_concurrency = 16;
  if (opt_.poll_ms <= 0) opt_.poll_ms = 50;
  if (opt_.poll_ms > 5000) opt_.poll_ms = 5000;
  if (opt_.max_scan_workflows == 0) opt_.max_scan_workflows = 16;
  if (opt_.max_inflight_per_workflow <= 0) opt_.max_inflight_per_workflow = 1;
  if (opt_.max_inflight_per_workflow > 64) opt_.max_inflight_per_workflow = 64;
  if (opt_.max_inflight_per_session < 0) opt_.max_inflight_per_session = 0;
  if (opt_.max_inflight_per_session > 1024) opt_.max_inflight_per_session = 1024;
}

WorkflowEngine::~WorkflowEngine() {
  stop();
}

std::string WorkflowEngine::json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

bool WorkflowEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_.load()) return true;
  if (!db_ || !db_->is_open()) {
    if (out_error) *out_error = "workflow engine requires an open AgentDb";
    return false;
  }
  if (!cfg_snapshot_ || !ocfg_from_cfg_) {
    if (out_error) *out_error = "workflow engine missing cfg snapshot function(s)";
    return false;
  }

  // Recovery (best-effort).
  {
    std::string err;
    (void)db_->recover_inflight_workflows(unix_ms_now(), &err);
  }

  stop_.store(false);
  running_.store(true);
  workers_.clear();
  workers_.reserve((size_t)opt_.max_concurrency);
  for (int i = 0; i < opt_.max_concurrency; i++) {
    workers_.emplace_back([this]() { worker_main(); });
  }
  return true;
}

void WorkflowEngine::stop() {
  stop_.store(true);
  if (!running_.load()) return;
  for (auto& th : workers_) {
    if (th.joinable()) th.join();
  }
  workers_.clear();
  running_.store(false);
}

void WorkflowEngine::worker_main() {
  while (!stop_.load()) {
    const int64_t now = unix_ms_now();
    AgentDb::WorkflowRow wf;
    AgentDb::WorkflowTaskRow task;
    std::string err;
    if (pick_and_claim_one(now, &wf, &task, &err)) {
      execute_claimed_task(wf, task);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
  }
}

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
  const size_t session_start = ns == 0 ? 0 : (size_t)(rr_cursor_.fetch_add(1) % (uint64_t)ns);

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

  return false;
}

void WorkflowEngine::execute_claimed_task(const AgentDb::WorkflowRow& wf, const AgentDb::WorkflowTaskRow& task) {
  const int64_t now = unix_ms_now();

  // Refresh workflow view (cancel_requested/deadline can change after claim).
  AgentDb::WorkflowRow wf_latest = wf;
  {
    AgentDb::WorkflowRow cur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &cur, &werr)) {
      wf_latest = std::move(cur);
    }
  }

  WorkflowRunCancelCtx cancel_ctx;
  cancel_ctx.db = db_;
  cancel_ctx.workflow_id = wf.workflow_id;
  cancel_ctx.deadline_unix_ms = workflow_deadline_unix_ms_best_effort(wf_latest);

  auto cancel_task_now = [&](const std::string& reason, const std::string& event_reason) {
    AgentDb::WorkflowTaskRow upd = task;
    upd.status = "cancelled";
    upd.updated_unix_ms = now;
    upd.finished_unix_ms = now;
    upd.ready_unix_ms = 0;
    upd.error = reason.empty() ? "cancelled" : reason;
    {
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["cancelled"] = true;
      out["assistant_text"] = "";
      out["error"] = upd.error;
      upd.result_json = json_stringify_compact(out);
    }
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["reason"] = event_reason;
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    maybe_finalize_workflow(wf.workflow_id);
  };

  // If cancellation/deadline is already in effect, do not execute provider/tool calls.
  if (wf_latest.cancel_requested) {
    cancel_task_now("cancelled", "cancel_requested");
    return;
  }
  if (cancel_ctx.deadline_unix_ms > 0 && now > cancel_ctx.deadline_unix_ms) {
    // Ensure the workflow row reflects the deadline cancellation even if no other worker is scanning.
    AgentDb::WorkflowRow upd = wf_latest;
    upd.cancel_requested = true;
    upd.updated_unix_ms = now;
    if (upd.error.empty()) upd.error = "deadline exceeded";
    (void)db_->upsert_workflow(upd, nullptr);
    cancel_task_now("deadline exceeded", "deadline_exceeded");
    return;
  }

  // Resolve template vars from completed tasks (assistant_text only).
  std::unordered_map<std::string, std::string> assistant_by_task;
  std::unordered_map<std::string, Json::Value> result_json_by_task;
  {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    std::string err;
    if (db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      for (const auto& t : tasks) {
        if (t.task_id.empty()) continue;
        if (t.status != "done" && t.status != "error") continue;
        if (t.result_json.empty()) continue;
        Json::Value r;
        std::string perr;
        if (!json_parse_any_value(t.result_json, &r, &perr) || !r.isObject()) continue;
        const std::string a = json_get_string(r, "assistant_text");
        if (!a.empty()) assistant_by_task[t.task_id] = a;
        result_json_by_task[t.task_id] = r;
      }
    }
  }

  // Build final run request body, applying template expansion.
  std::string request_body = task.request_json;
  Json::Value rr;
  std::string perr;
  Json::Value out;
  if (json_parse_any_value(task.request_json, &rr, &perr) && rr.isObject()) {
    std::vector<std::string> tmpl_errors;
    (void)workflow_expand_templates_for_task_request(&rr, assistant_by_task, result_json_by_task, &tmpl_errors);
    if (!tmpl_errors.empty()) {
      out = Json::Value(Json::objectValue);
      out["ok"] = false;
      out["assistant_text"] = "";
      out["error"] = "template expansion failed";
      Json::Value arr(Json::arrayValue);
      for (const auto& e : tmpl_errors) arr.append(e);
      out["template_errors"] = arr;
    }
    request_body = json_stringify_compact(rr);
  }

  const DaemonConfig cfg = cfg_snapshot_();
  const OpenAIClientConfig ocfg = ocfg_from_cfg_(cfg);
  std::string kind;
  if (rr.isObject()) kind = json_get_string(rr, "kind");
  if (!out.isNull()) {
    // Template expansion failed: do not execute provider calls. `out` is already populated as an error.
  } else if (kind == "avm_capsule") {
    const Json::Value cap = rr.isMember("capsule") && rr["capsule"].isObject() ? rr["capsule"] : Json::Value(Json::nullValue);
    Json::Value avm_out;
    std::string aerr;
    (void)avm_capsule_run_to_json(cfg, cap, &avm_out, &aerr);
    out = Json::Value(Json::objectValue);
    out["kind"] = "avm_capsule";
    out["avm"] = avm_out;
    const bool ok = avm_out.isObject() && avm_out.isMember("ok") && avm_out["ok"].isBool() && avm_out["ok"].asBool();
    out["ok"] = ok;
    if (!ok) {
      const std::string err =
        avm_out.isObject() && avm_out.isMember("error") && avm_out["error"].isString() ? avm_out["error"].asString()
        : (!aerr.empty() ? aerr : "avm capsule_run failed");
      out["error"] = err;
    }
    // For template expansion convenience, surface a bounded assistant_text (best-effort).
    if (avm_out.isObject() && avm_out.isMember("result_hash") && avm_out["result_hash"].isString()) {
      out["assistant_text"] = avm_out["result_hash"].asString();
    } else if (avm_out.isObject() && avm_out.isMember("run")) {
      out["assistant_text"] = json_stringify_compact_local(avm_out["run"]);
    } else {
      out["assistant_text"] = "";
    }
  } else if (kind == "aggregate") {
    const Json::Value agg = rr.isMember("aggregate") && rr["aggregate"].isObject() ? rr["aggregate"] : Json::Value(Json::nullValue);
    std::string aerr;
    out = workflow_aggregate_to_json(agg, result_json_by_task, &aerr);
    if (!aerr.empty() && (!out.isMember("error") || !out["error"].isString())) out["error"] = aerr;
  } else if (kind == "memory_put") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_put";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else if (lower_copy(trim_copy(cfg.tools)) != "host") {
      out["error"] = "memory_put requires --tools host";
    } else if (cfg.host_policy != HostToolsetPolicyMode::Full) {
      out["error"] = "memory_put requires host_policy=full";
    } else {
      const Json::Value mp =
        rr.isMember("memory_put") && rr["memory_put"].isObject() ? rr["memory_put"] : Json::Value(Json::nullValue);
      if (!mp.isObject()) {
        out["error"] = "memory_put missing memory_put object";
      } else {
        const std::string path =
          mp.isMember("path") && mp["path"].isString() ? trim_copy(mp["path"].asString()) : "STRUCTURED.md";
        const Json::Value entries = mp.isMember("entries") ? mp["entries"] : Json::Value(Json::nullValue);
        if (!entries.isArray() || entries.empty()) {
          out["error"] = "memory_put.entries must be a non-empty array";
        } else {
          std::string corr = std::string("workflow:") + wf.workflow_id + " task:" + task.task_id;
          if (!wf.session_id.empty()) corr += std::string(" session:") + wf.session_id;
          const std::string ttrace =
            rr.isMember("trace_id") && rr["trace_id"].isString() ? trim_copy(rr["trace_id"].asString()) : "";
          if (!ttrace.empty()) corr += std::string(" trace:") + ttrace;

          Json::Value tool_entries(Json::arrayValue);
          int valid = 0;
          for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
            const Json::Value e = entries[i];
            if (!e.isObject()) continue;
            const std::string key = e.isMember("key") && e["key"].isString() ? trim_copy(e["key"].asString()) : "";
            const std::string value = e.isMember("value") && e["value"].isString() ? e["value"].asString() : "";
            if (key.empty() || value.empty()) continue;
            valid++;

            const std::string src =
              e.isMember("source") && e["source"].isString() ? trim_copy(e["source"].asString()) : "";
            if (src.empty()) {
              Json::Value e2 = e;
              e2["source"] = corr;
              tool_entries.append(e2);
            } else {
              tool_entries.append(e);
              if (!corr.empty() && src != corr) {
                Json::Value e3 = e;
                e3["source"] = corr;
                tool_entries.append(e3);
              }
            }
          }

          if (valid == 0) {
            out["error"] = "memory_put.entries has no valid entries (each entry requires key + value)";
          } else {
            Json::Value args(Json::objectValue);
            args["path"] = path.empty() ? "STRUCTURED.md" : path;
            args["entries"] = tool_entries;
            const bool checkpoint =
              !mp.isMember("checkpoint") || (mp["checkpoint"].isBool() && mp["checkpoint"].asBool());
            args["checkpoint"] = checkpoint;
            int keep_checkpoints = cfg.memory_consolidate_keep_checkpoints > 0 ? cfg.memory_consolidate_keep_checkpoints : 100;
            if (mp.isMember("keep_checkpoints") && mp["keep_checkpoints"].isInt()) {
              keep_checkpoints = std::max(1, mp["keep_checkpoints"].asInt());
            }
            args["keep_checkpoints"] = keep_checkpoints;

            HostToolsetConfig hcfg;
            hcfg.root_dir = cfg.state_dir;
            hcfg.policy = HostToolsetPolicyMode::Full;
            hcfg.enable_process_exec = false;
            hcfg.allow_symlinks = true;
            hcfg.sessions_root_dir = !cfg.sessions_root_dir.empty() ? cfg.sessions_root_dir : sessions_root_dir_;
            hcfg.session_id = wf.session_id;

            agent_tool_registry_t* reg = nullptr;
            agent_tool_executor_t exec{};
            const agent_status_t st = toolset_host_create(hcfg, &reg, &exec);
            if (st != AGENT_OK || !reg || !exec.execute) {
              if (reg) agent_tool_registry_destroy(reg);
              toolset_host_destroy(&exec);
              out["error"] = "failed to create host toolset";
            } else {
              Json::StreamWriterBuilder wb;
              wb["indentation"] = "";
              const std::string req = Json::writeString(wb, args);

              agent_string_t out_s{};
              const agent_status_t est = exec.execute(exec.ctx, "memory_put", req.c_str(), &out_s);
              const std::string resp_s = (out_s.data && out_s.len) ? std::string(out_s.data, out_s.len) : std::string();
              agent_string_free(&out_s);

              agent_tool_registry_destroy(reg);
              toolset_host_destroy(&exec);

              if (est != AGENT_OK) {
                out["error"] = "memory_put failed";
              } else {
                Json::Value resp(Json::objectValue);
                std::string rerr;
                if (!json_parse_any_value(resp_s, &resp, &rerr) || !resp.isObject()) {
                  out["error"] = "failed to parse memory_put response";
                  out["parse_error"] = rerr;
                } else {
                  out["memory_put_response"] = resp;
                  const bool ok =
                    resp.isMember("ok") && resp["ok"].isBool() && resp["ok"].asBool();
                  out["ok"] = ok;
                  out["path"] = args["path"];
                  if (!ok) {
                    const std::string err =
                      resp.isMember("error") && resp["error"].isString() ? resp["error"].asString() : "memory_put failed";
                    out["error"] = err;
                    out["assistant_text"] = "";
                  } else {
                    const std::string output =
                      resp.isMember("data") && resp["data"].isObject() && resp["data"].isMember("output") && resp["data"]["output"].isString()
                      ? resp["data"]["output"].asString()
                      : (std::string("memory_put: ") + args["path"].asString());
                    out["assistant_text"] = output;
                  }
                }
              }
            }
          }
        }
      }
    }
  } else if (kind == "memory_consolidate") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "memory_consolidate";
    out["ok"] = false;
    out["assistant_text"] = "";

    if (workflow_run_should_cancel(&cancel_ctx)) {
      out["cancelled"] = true;
      out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
    } else if (lower_copy(trim_copy(cfg.tools)) != "host") {
      out["error"] = "memory_consolidate requires --tools host";
    } else if (cfg.host_policy != HostToolsetPolicyMode::Full) {
      out["error"] = "memory_consolidate requires host_policy=full";
    } else {
      const Json::Value mc =
        rr.isMember("memory_consolidate") && rr["memory_consolidate"].isObject() ? rr["memory_consolidate"] : Json::Value(Json::objectValue);

      MemoryConsolidateOptions opt;
      opt.daily_days = cfg.memory_consolidate_daily_days;
      opt.keep_checkpoints = cfg.memory_consolidate_keep_checkpoints;

      if (mc.isMember("daily_days") && mc["daily_days"].isInt()) {
        opt.daily_days = std::max(0, mc["daily_days"].asInt());
      }
      if (mc.isMember("keep_checkpoints") && mc["keep_checkpoints"].isInt()) {
        opt.keep_checkpoints = std::max(1, mc["keep_checkpoints"].asInt());
      }
      if (mc.isMember("max_entries") && mc["max_entries"].isInt()) {
        opt.max_entries = std::max(1, mc["max_entries"].asInt());
      }
      if (mc.isMember("dry_run") && mc["dry_run"].isBool()) {
        opt.dry_run = mc["dry_run"].asBool();
      }

      Json::Value report;
      std::string merr;
      if (!memory_consolidate_once(cfg, opt, &report, &merr)) {
        out["ok"] = false;
        out["error"] = merr.empty() ? "memory consolidation failed" : merr;
      } else {
        out["ok"] = true;
        out["report"] = report;
        if (report.isObject() && report.isMember("output") && report["output"].isString()) {
          out["assistant_text"] = report["output"].asString();
        } else {
          out["assistant_text"] = "memory_consolidate: ok";
        }
      }
    }
  } else if (kind == "delay") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "delay";
    out["ok"] = false;

    int64_t delay_ms = 0;
    if (rr.isMember("delay_ms") && (rr["delay_ms"].isInt64() || rr["delay_ms"].isUInt64() || rr["delay_ms"].isInt())) {
      delay_ms = rr["delay_ms"].asInt64();
    } else if (rr.isMember("delay_ms")) {
      out["error"] = "delay_ms must be an integer";
    }

    if (!out.isMember("error")) {
      if (delay_ms < 0) {
        out["error"] = "delay_ms must be >= 0";
      } else {
        if (delay_ms > 600000) delay_ms = 600000;
        if (delay_ms > 0) {
          const int64_t until = unix_ms_now() + delay_ms;
          while (!stop_.load()) {
            if (workflow_run_should_cancel(&cancel_ctx)) {
              out["ok"] = false;
              out["cancelled"] = true;
              out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
              break;
            }
            const int64_t now2 = unix_ms_now();
            if (now2 >= until) break;
            const int64_t remaining = until - now2;
            const int64_t chunk = std::min<int64_t>(50, remaining);
            if (chunk > 0) std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
          }
        }

        if (!out.isMember("cancelled")) {
          out["ok"] = true;
          out["delay_ms"] = (Json::Int64)delay_ms;
        }

        if (out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool() && rr.isMember("result") && rr["result"].isObject()) {
          const auto& r = rr["result"];
          for (const auto& k : r.getMemberNames()) {
            if (k == "ok" || k == "kind" || k == "delay_ms") continue;
            out[k] = r[k];
          }
        } else if (rr.isMember("result") && !rr["result"].isNull()) {
          // Keep behavior strict: if provided, result must be an object.
          out["ok"] = false;
          out["error"] = "result must be an object";
        }

        if (out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool()) {
          if (!out.isMember("assistant_text") || !out["assistant_text"].isString()) {
            out["assistant_text"] = std::string("delay:") + std::to_string((long long)delay_ms);
          }
        }
      }
    }
  } else if (kind == "delegate") {
    out = Json::Value(Json::objectValue);
    out["kind"] = "delegate";
    out["ok"] = false;

    const Json::Value del = rr.isMember("delegate") && rr["delegate"].isObject() ? rr["delegate"] : Json::Value(Json::nullValue);
    if (!del.isObject()) {
      out["error"] = "delegate missing delegate object";
    } else {
      const bool stop_on_ok =
        del.isMember("stop_on_ok") && del["stop_on_ok"].isBool() ? del["stop_on_ok"].asBool() : true;

      const Json::Value attempts = del.isMember("attempts") && del["attempts"].isArray() ? del["attempts"] : Json::Value(Json::nullValue);
      if (!attempts.isArray() || attempts.empty()) {
        out["error"] = "delegate.attempts must be a non-empty array";
      } else {
        auto clamp_text = [](const std::string& s, size_t max_chars) -> std::string {
          if (s.size() <= max_chars) return s;
          return s.substr(0, max_chars);
        };

        Json::Value del_out(Json::objectValue);
        del_out["stop_on_ok"] = stop_on_ok;
        Json::Value arr(Json::arrayValue);

        bool any_ok = false;
        std::string chosen_id;
        std::string chosen_text;
        std::string last_err;

        for (Json::ArrayIndex i = 0; i < attempts.size(); i++) {
          if (workflow_run_should_cancel(&cancel_ctx)) {
            out["cancelled"] = true;
            out["ok"] = false;
            out["error"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled";
            break;
          }

          const Json::Value a = attempts[i];
          if (!a.isObject()) continue;

          const std::string aid = a.isMember("id") && a["id"].isString() ? a["id"].asString() : ("att_" + std::to_string((int)i));
          const Json::Value areq = a.isMember("request") && a["request"].isObject() ? a["request"] : Json::Value(Json::nullValue);
          if (!areq.isObject()) {
            Json::Value row(Json::objectValue);
            row["id"] = aid;
            row["ok"] = false;
            row["run_ok"] = false;
            row["expect_ok"] = true;
            row["error"] = "delegate attempt missing request object";
            arr.append(row);
            last_err = "delegate attempt missing request object";
            continue;
          }

          const std::string attempt_body = json_stringify_compact(areq);
          Json::Value r = run_request_to_json_internal_cancellable(
            cfg,
            ocfg,
            db_,
            tool_ext_or_null_,
            sessions_root_dir_,
            attempt_body,
            nullptr,
            workflow_run_should_cancel,
            &cancel_ctx
          );

          std::string expect_err2;
          bool expect_ok2 = true;
          if (a.isMember("expect") && a["expect"].isObject()) {
            const std::string expect_json2 = json_stringify_compact(a["expect"]);
            expect_ok2 = apply_expectations(expect_json2, r, &expect_err2);
          }

          const bool run_ok2 = r.isObject() && r.isMember("ok") && r["ok"].isBool() && r["ok"].asBool();
          const bool ok2 = run_ok2 && expect_ok2;
          const std::string atext = clamp_text(json_get_string(r, "assistant_text"), 8192);

          Json::Value row(Json::objectValue);
          row["id"] = aid;
          row["ok"] = ok2;
          row["run_ok"] = run_ok2;
          row["expect_ok"] = expect_ok2;
          if (!atext.empty()) row["assistant_text"] = atext;
          const std::string err = json_get_string(r, "error");
          if (!err.empty()) row["error"] = err;
          if (!expect_ok2) row["expect_error"] = expect_err2;
          if (r.isObject() && r.isMember("http_status") && (r["http_status"].isInt() || r["http_status"].isInt64())) {
            row["http_status"] = r["http_status"];
          }
          arr.append(row);

          if (!err.empty()) last_err = err;
          if (!expect_ok2 && last_err.empty()) last_err = expect_err2;

          if (ok2 && !any_ok) {
            any_ok = true;
            chosen_id = aid;
            chosen_text = atext;
          }

          if (ok2 && stop_on_ok) break;
        }

        del_out["attempts"] = arr;
        del_out["attempts_total"] = (Json::Int64)attempts.size();
        del_out["attempts_run"] = (Json::Int64)arr.size();
        if (!chosen_id.empty()) del_out["chosen_id"] = chosen_id;
        out["delegate"] = del_out;

        if (out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool()) {
          // already populated
        } else if (any_ok) {
          out["ok"] = true;
          out["assistant_text"] = chosen_text;
        } else {
          out["ok"] = false;
          out["assistant_text"] = "";
          out["error"] = last_err.empty() ? "delegate attempts all failed" : last_err;
        }
      }
    }
  } else if (kind == "edge_invoke") {
    Json::Value e = rr.isMember("edge") && rr["edge"].isObject() ? rr["edge"] : Json::Value(Json::nullValue);
    out = Json::Value(Json::objectValue);
    out["kind"] = "edge_invoke";
    out["ok"] = false;

    // Best-effort: reuse chosen node_id from a previous attempt to avoid cross-node duplication.
    if ((!e.isObject() || !e.isMember("node_id") || !e["node_id"].isString() || e["node_id"].asString().empty()) && !task.result_json.empty()) {
      Json::Value prev;
      std::string perr2;
      if (json_parse_any_value(task.result_json, &prev, &perr2) && prev.isObject()) {
        const std::string prev_node =
          prev.isMember("edge") && prev["edge"].isObject() && prev["edge"].isMember("node_id") && prev["edge"]["node_id"].isString()
          ? prev["edge"]["node_id"].asString()
          : "";
        if (!prev_node.empty()) {
          if (!e.isObject()) e = Json::Value(Json::objectValue);
          e["node_id"] = prev_node;
        }
      }
    }

    if (!db_ || !db_->is_open()) {
      out["error"] = "db not available";
      out["retryable"] = true;
      out["retry_in_ms"] = 250;
    } else if (!e.isObject()) {
      out["error"] = "edge config must be an object";
    } else {
      std::string node_id = e.isMember("node_id") && e["node_id"].isString() ? e["node_id"].asString() : "";

      if (node_id.empty() && e.isMember("match_any") && e["match_any"].isObject()) {
        const auto& m = e["match_any"];
        auto read_arr = [&](const char* k, std::vector<std::string>* outv) {
          if (!outv) return;
          outv->clear();
          if (!m.isMember(k) || !m[k].isArray()) return;
          for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
            if (!m[k][i].isString()) continue;
            const std::string s = m[k][i].asString();
            if (!s.empty()) outv->push_back(s);
          }
        };
        std::vector<std::string> requires_tools;
        std::vector<std::string> tags_all;
        std::vector<std::string> tags_any;
        std::vector<std::string> tags_none;
        read_arr("requires_tools", &requires_tools);
        read_arr("tags_all", &tags_all);
        read_arr("tags_any", &tags_any);
        read_arr("tags_none", &tags_none);
        (void)edge_select_node_match_any(db_, requires_tools, tags_all, tags_any, tags_none, &node_id);
        if (!node_id.empty()) e["node_id"] = node_id;
      }

      const std::string mode = e.isMember("mode") && e["mode"].isString() ? e["mode"].asString() : "invoke";
      const std::string tool_name = e.isMember("tool") && e["tool"].isString() ? e["tool"].asString() : "";
      const Json::Value args = e.isMember("args") && e["args"].isObject() ? e["args"] : Json::Value(Json::nullValue);

      int64_t deadline_utc_ms = 0;
      if (e.isMember("deadline_utc_ms") && (e["deadline_utc_ms"].isInt64() || e["deadline_utc_ms"].isUInt64())) {
        deadline_utc_ms = e["deadline_utc_ms"].isInt64() ? e["deadline_utc_ms"].asInt64() : (int64_t)e["deadline_utc_ms"].asUInt64();
      } else if (e.isMember("timeout_ms") && (e["timeout_ms"].isInt64() || e["timeout_ms"].isUInt64() || e["timeout_ms"].isInt())) {
        const int64_t tmo = e["timeout_ms"].asInt64();
        const int64_t tmo2 = std::max<int64_t>(100, std::min<int64_t>(600000, tmo));
        deadline_utc_ms = edge_unix_ms_now() + tmo2;
      } else {
        deadline_utc_ms = edge_unix_ms_now() + 5000;
      }

      std::unordered_set<std::string> allow_hazards;
      if (e.isMember("allow_hazards") && e["allow_hazards"].isArray()) {
        for (Json::ArrayIndex i = 0; i < e["allow_hazards"].size(); i++) {
          if (e["allow_hazards"][i].isString()) allow_hazards.insert(e["allow_hazards"][i].asString());
        }
      }
      const bool allow_high_side_effect =
        e.isMember("allow_high_side_effect") && e["allow_high_side_effect"].isBool() ? e["allow_high_side_effect"].asBool() : false;

      std::string idempotency_key = e.isMember("idempotency_key") && e["idempotency_key"].isString() ? e["idempotency_key"].asString() : "";
      if (idempotency_key.empty()) idempotency_key = wf.workflow_id + ":" + task.task_id;

      if (node_id.empty()) {
        out["error"] = "missing node_id (or match_any did not select)";
      } else {
        Json::Value payload(Json::objectValue);
        if (mode == "invoke") {
          if (tool_name.empty() || !args.isObject()) {
            out["error"] = "missing tool/args";
            out["edge"] = e;
            // fall through to persist out
            payload = Json::Value(Json::nullValue);
          } else {
            payload["tool"] = tool_name;
            payload["args"] = args;
          }
        } else if (mode == "agent") {
          if (e.isMember("payload") && e["payload"].isObject()) {
            payload = e["payload"];
          } else if (e.isMember("prompt") && e["prompt"].isString() && !e["prompt"].asString().empty()) {
            payload["prompt"] = e["prompt"].asString();
          } else {
            out["error"] = "missing edge.payload (object) or edge.prompt (string) for mode=agent";
            out["edge"] = e;
            payload = Json::Value(Json::nullValue);
          }
        } else {
          out["error"] = "unsupported edge mode (expected invoke|agent)";
          out["edge"] = e;
          payload = Json::Value(Json::nullValue);
        }

        if (!payload.isObject()) {
          // Invalid payload/mode; no enqueue.
        } else {
          // Enqueue TASK_ASSIGN (idempotent via UNIQUE(node_id,idempotency_key) and PK(task_id,step_id)).

          int64_t outbox_id = 0;
          bool deduped = false;
          std::string derr;
          int http = 500;
          const bool enq_ok = edge_enqueue_task_assign(
            db_,
            node_id,
            wf.workflow_id,
            task.task_id,
            idempotency_key,
            mode,
            deadline_utc_ms,
            task.attempt,
            payload,
            allow_hazards,
            allow_high_side_effect,
            /*enforce_safety=*/true,
            /*enforce_rate_limit=*/true,
            &outbox_id,
            &deduped,
            &derr,
            &http);

          Json::Value edge(Json::objectValue);
          edge["node_id"] = node_id;
          edge["task_id"] = wf.workflow_id;
          edge["step_id"] = task.task_id;
          edge["idempotency_key"] = idempotency_key;
          edge["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
          edge["outbox_id"] = (Json::Int64)outbox_id;
          edge["deduped"] = deduped;
          edge["mode"] = mode;
          out["edge"] = edge;

          if (!enq_ok) {
            out["error"] = derr.empty() ? "failed to enqueue edge task" : derr;
          } else {
            AgentDb::EdgeTaskRow tr;
            std::string terr;
            if (!db_->get_edge_task(wf.workflow_id, task.task_id, &tr, &terr)) {
              out["error"] = "edge task not found after enqueue";
              out["retryable"] = true;
              out["retry_in_ms"] = 100;
            } else {
              out["edge_state"] = tr.state;
              if (!tr.result_json.empty()) {
                Json::Value v;
                std::string perr3;
                if (json_parse_any(tr.result_json, &v, &perr3)) out["edge_result"] = v;
              }
              if (!tr.error.empty()) out["edge_error"] = tr.error;

              if (tr.state == "SUCCEEDED") {
                out["ok"] = true;
                if (out.isMember("edge_result")) out["assistant_text"] = json_stringify_compact_local(out["edge_result"]);
                else out["assistant_text"] = "edge:SUCCEEDED";
              } else if (tr.state == "FAILED") {
                out["ok"] = false;
                out["error"] = tr.error.empty() ? "edge task failed" : tr.error;
              } else {
                out["ok"] = false;
                out["error"] = "edge task pending";
                out["retryable"] = true;
                out["retry_in_ms"] = 100;
                out["assistant_text"] = "edge:" + tr.state;
              }
            }
          }
        }
      }
    }
  } else {
    out = run_request_to_json_internal_cancellable(
      cfg,
      ocfg,
      db_,
      tool_ext_or_null_,
      sessions_root_dir_,
      request_body,
      nullptr,
      workflow_run_should_cancel,
      &cancel_ctx
    );
  }

  std::string expect_err;
  const bool expect_ok = apply_expectations(task.expect_json, out, &expect_err);
  const bool run_ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
  const bool run_cancelled =
    out.isObject() && out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool();

  AgentDb::WorkflowTaskRow upd = task;
  upd.updated_unix_ms = now;
  upd.finished_unix_ms = now;
  upd.result_json = json_stringify_compact(out);

  if (run_cancelled) {
    upd.status = "cancelled";
    upd.ready_unix_ms = 0;
    upd.error =
      out.isObject() && out.isMember("error") && out["error"].isString() && !out["error"].asString().empty()
      ? out["error"].asString()
      : (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline exceeded" : "cancelled");
    (void)db_->upsert_workflow_task(upd, nullptr);

    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["reason"] = cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded ? "deadline_exceeded" : "cancel_requested";
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }

    // Ensure the workflow is marked cancelled when a running task cooperatively cancels.
    AgentDb::WorkflowRow wcur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &wcur, &werr)) {
      if (!wcur.cancel_requested || (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded && wcur.error != "deadline exceeded")) {
        wcur.cancel_requested = true;
        wcur.updated_unix_ms = now;
        if (cancel_ctx.reason == WorkflowCancelReason::DeadlineExceeded) wcur.error = "deadline exceeded";
        else if (wcur.error.empty()) wcur.error = "cancelled";
        (void)db_->upsert_workflow(wcur, nullptr);
      }
    }

    maybe_finalize_workflow(wf.workflow_id);
    return;
  }

  if (run_ok && expect_ok) {
    upd.status = "done";
    upd.error.clear();
    upd.ready_unix_ms = 0;
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["finished_unix_ms"] = (Json::Int64)upd.finished_unix_ms;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    maybe_finalize_workflow(wf.workflow_id);
    return;
  }

  const bool can_retry = upd.attempt < std::max(1, upd.max_attempts);
  upd.error = !expect_ok ? expect_err : (out.isObject() && out.isMember("error") && out["error"].isString() ? out["error"].asString() : "error");

  bool wf_cancel_requested = wf_latest.cancel_requested;
  {
    AgentDb::WorkflowRow cur;
    std::string werr;
    if (db_->get_workflow(wf.workflow_id, &cur, &werr)) {
      wf_cancel_requested = cur.cancel_requested;
    }
  }

  if (can_retry && !wf_cancel_requested) {
    upd.status = "queued";
    int64_t delay_ms = retry_backoff_ms(upd.attempt);
    if (out.isObject() && out.isMember("retryable") && out["retryable"].isBool() && out["retryable"].asBool() &&
        out.isMember("retry_in_ms") && (out["retry_in_ms"].isInt64() || out["retry_in_ms"].isUInt64() || out["retry_in_ms"].isInt())) {
      delay_ms = out["retry_in_ms"].asInt64();
      if (delay_ms < 0) delay_ms = 0;
      if (delay_ms > 60 * 1000) delay_ms = 60 * 1000;
    }
    upd.ready_unix_ms = now + delay_ms;
    (void)db_->upsert_workflow_task(upd, nullptr);
    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = wf.workflow_id;
      d["task_id"] = upd.task_id;
      d["status"] = upd.status;
      d["attempt"] = upd.attempt;
      d["max_attempts"] = upd.max_attempts;
      d["ready_unix_ms"] = (Json::Int64)upd.ready_unix_ms;
      if (!upd.error.empty()) d["error"] = upd.error;
      d["ts_unix_ms"] = (Json::Int64)now;
      insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
    }
    return;
  }

  upd.status = "error";
  upd.ready_unix_ms = 0;
  (void)db_->upsert_workflow_task(upd, nullptr);
  {
    Json::Value d(Json::objectValue);
    d["workflow_id"] = wf.workflow_id;
    d["task_id"] = upd.task_id;
    d["status"] = upd.status;
    d["attempt"] = upd.attempt;
    d["max_attempts"] = upd.max_attempts;
    if (!upd.error.empty()) d["error"] = upd.error;
    d["ts_unix_ms"] = (Json::Int64)now;
    insert_workflow_event_best_effort(db_, wf.workflow_id, upd.task_id, "task_status", now, d);
  }
  maybe_finalize_workflow(wf.workflow_id);
}

void WorkflowEngine::maybe_finalize_workflow(const std::string& workflow_id) {
  if (workflow_id.empty()) return;
  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_->get_workflow(workflow_id, &wf, &err)) return;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  if (!db_->list_workflow_tasks(workflow_id, &tasks, &err)) return;

  bool any_running = false;
  bool any_queued = false;
  bool any_hard_error = false;
  std::string first_error;
  bool all_terminal = true;
  for (const auto& t : tasks) {
    if (t.status == "running") any_running = true;
    if (t.status == "queued") any_queued = true;
    if (t.status == "error" && !t.allow_error) {
      any_hard_error = true;
      if (first_error.empty()) first_error = t.error;
    }
    const bool terminal = workflow_is_terminal_status(t.status);
    if (!terminal) all_terminal = false;
  }

  std::string new_status = wf.status;
  std::string wf_error = wf.error;

  if (any_hard_error) {
    new_status = "error";
    if (wf_error.empty()) wf_error = first_error.empty() ? "workflow task failed" : first_error;
  } else if (wf.cancel_requested) {
    if (all_terminal) {
      new_status = "cancelled";
      if (wf_error.empty()) wf_error = "cancelled";
    } else {
      new_status = "running";
    }
  } else if (all_terminal && !tasks.empty()) {
    new_status = "done";
    wf_error.clear();
  } else if (any_running) {
    new_status = "running";
  } else if (any_queued) {
    new_status = "queued";
  }

  // Persist final aggregation when terminal.
  std::string result_json;
  if (new_status == "done" || new_status == "error" || new_status == "cancelled") {
    Json::Value r(Json::objectValue);
    r["workflow_id"] = workflow_id;
    r["ok"] = (new_status == "done");
    r["status"] = new_status;
    if (!wf.trace_id.empty()) r["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) r["session_id"] = wf.session_id;
    if (!wf_error.empty()) r["error"] = wf_error;
    Json::Value arr(Json::arrayValue);
    Json::Value by_task(Json::objectValue);
    for (const auto& t : tasks) {
      Json::Value row(Json::objectValue);
      row["task_id"] = t.task_id;
      row["status"] = t.status;
      row["attempt"] = t.attempt;
      row["max_attempts"] = t.max_attempts;
      row["ready_unix_ms"] = (Json::Int64)t.ready_unix_ms;
      row["started_unix_ms"] = (Json::Int64)t.started_unix_ms;
      row["finished_unix_ms"] = (Json::Int64)t.finished_unix_ms;
      if (!t.error.empty()) row["error"] = t.error;
      if (!t.depends_on_json.empty()) {
        Json::Value deps;
        std::string derr;
        if (json_parse_any_value(t.depends_on_json, &deps, &derr) && deps.isArray()) row["depends_on"] = deps;
      }
      arr.append(row);

      if (!t.result_json.empty()) {
        Json::Value rr;
        std::string rerr;
        if (json_parse_any_value(t.result_json, &rr, &rerr)) {
          by_task[t.task_id] = rr;
        }
      }
    }
    r["tasks"] = arr;
    r["results_by_task"] = by_task;
    result_json = json_stringify_compact(r);
  }

  if (new_status != wf.status || wf_error != wf.error || (!result_json.empty() && result_json != wf.result_json)) {
    const std::string prev = wf.status;
    wf.status = new_status;
    wf.updated_unix_ms = unix_ms_now();
    wf.error = wf_error;
    if (!result_json.empty()) wf.result_json = result_json;
    (void)db_->upsert_workflow(wf, nullptr);

    {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      d["prev_status"] = prev;
      d["status"] = new_status;
      d["cancel_requested"] = wf.cancel_requested;
      if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
      if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
      if (!wf.error.empty()) d["error"] = wf.error;
      d["ts_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
      insert_workflow_event_best_effort(db_, workflow_id, "", "workflow_status", wf.updated_unix_ms, d);
    }

    if (new_status == "done" || new_status == "error" || new_status == "cancelled") {
      Json::Value d(Json::objectValue);
      d["workflow_id"] = workflow_id;
      d["ok"] = (new_status == "done");
      d["status"] = new_status;
      if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
      if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
      if (!wf.error.empty()) d["error"] = wf.error;
      d["result_json_present"] = !wf.result_json.empty();
      d["ts_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
      insert_workflow_event_best_effort(db_, workflow_id, "", "workflow_done", wf.updated_unix_ms, d);
    }
  }
}

}  // namespace agentd
