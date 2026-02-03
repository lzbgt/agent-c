#include "workflow_engine.h"

#include "json_util.h"
#include "run_endpoints.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
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
static std::string expand_prompt_templates(
  const std::string& prompt,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task
) {
  if (prompt.find("${task.") == std::string::npos) return prompt;
  std::string out;
  out.reserve(prompt.size() + 64);

  size_t i = 0;
  while (i < prompt.size()) {
    const size_t p = prompt.find("${task.", i);
    if (p == std::string::npos) {
      out.append(prompt, i, std::string::npos);
      break;
    }
    out.append(prompt, i, p - i);
    const size_t end = prompt.find("}", p);
    if (end == std::string::npos) {
      // Unclosed token; keep literal.
      out.append(prompt, p, std::string::npos);
      break;
    }

    const std::string token = prompt.substr(p + 2, end - (p + 2)); // strip ${ ... }
    // token: task.<id>.assistant_text
    const std::string prefix = "task.";
    if (token.rfind(prefix, 0) != 0) {
      out.append(prompt, p, (end - p) + 1);
      i = end + 1;
      continue;
    }
    const std::string rest = token.substr(prefix.size());
    const std::string suffix = ".assistant_text";
    if (rest.size() <= suffix.size() || rest.rfind(suffix) != (rest.size() - suffix.size())) {
      out.append(prompt, p, (end - p) + 1);
      i = end + 1;
      continue;
    }
    const std::string task_id = rest.substr(0, rest.size() - suffix.size());
    auto it = assistant_text_by_task.find(task_id);
    if (it == assistant_text_by_task.end()) {
      // Missing dependency result; keep literal.
      out.append(prompt, p, (end - p) + 1);
    } else {
      out += it->second;
    }
    i = end + 1;
  }

  return out;
}

static bool json_pointer_get(const Json::Value& root, const std::string& ptr, const Json::Value** out) {
  if (!out) return false;
  *out = nullptr;
  if (ptr.empty()) {
    *out = &root;
    return true;
  }
  if (ptr[0] != '/') return false;
  const Json::Value* cur = &root;

  size_t i = 1;
  while (i <= ptr.size()) {
    size_t slash = ptr.find('/', i);
    if (slash == std::string::npos) slash = ptr.size();
    std::string seg = ptr.substr(i, slash - i);
    // unescape ~0 and ~1
    for (size_t j = 0; j + 1 < seg.size(); j++) {
      if (seg[j] == '~') {
        if (seg[j + 1] == '0') {
          seg.replace(j, 2, "~");
        } else if (seg[j + 1] == '1') {
          seg.replace(j, 2, "/");
        }
      }
    }

    if (cur->isObject()) {
      if (!cur->isMember(seg)) return false;
      cur = &((*cur)[seg]);
    } else if (cur->isArray()) {
      if (seg.empty()) return false;
      char* endp = nullptr;
      long long idx = std::strtoll(seg.c_str(), &endp, 10);
      if (!endp || *endp != '\0') return false;
      if (idx < 0) return false;
      if ((Json::ArrayIndex)idx >= cur->size()) return false;
      cur = &((*cur)[(Json::ArrayIndex)idx]);
    } else {
      return false;
    }

    if (slash == ptr.size()) break;
    i = slash + 1;
  }

  *out = cur;
  return true;
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

  std::vector<AgentDb::WorkflowRow> queued;
  std::vector<AgentDb::WorkflowRow> running;
  std::string err;
  if (!db_->list_workflows_by_status("queued", opt_.max_scan_workflows, &queued, &err)) {
    if (out_error) *out_error = err;
    return false;
  }
  if (!db_->list_workflows_by_status("running", opt_.max_scan_workflows, &running, &err)) {
    if (out_error) *out_error = err;
    return false;
  }

  std::vector<AgentDb::WorkflowRow> all;
  all.reserve(queued.size() + running.size());
  all.insert(all.end(), queued.begin(), queued.end());
  all.insert(all.end(), running.begin(), running.end());

  // De-dup by workflow_id while preserving "queued first" ordering.
  std::unordered_set<std::string> seen;
  std::vector<AgentDb::WorkflowRow> wfs;
  wfs.reserve(all.size());
  for (auto& wf : all) {
    if (wf.workflow_id.empty()) continue;
    if (seen.insert(wf.workflow_id).second) wfs.push_back(std::move(wf));
  }

  for (auto& wf : wfs) {
    if (stop_.load()) return false;
    if (wf.workflow_id.empty()) continue;
    if (wf.status != "queued" && wf.status != "running") continue;

    std::vector<AgentDb::WorkflowTaskRow> tasks;
    if (!db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      continue;
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
          any_change = true;
        }
      }
      if (any_change) {
        maybe_finalize_workflow(wf.workflow_id);
        // Keep scanning in case another workflow is runnable.
      }
      continue;
    }

    std::unordered_map<std::string, std::string> status_by_id;
    status_by_id.reserve(tasks.size());
    for (const auto& t : tasks) {
      status_by_id[t.task_id] = t.status;
    }

    for (auto& t : tasks) {
      if (t.status != "queued") continue;
      if (t.ready_unix_ms > 0 && now_unix_ms < t.ready_unix_ms) continue;

      const std::vector<std::string> deps = parse_dep_ids(t.depends_on_json);
      bool deps_ok = true;
      for (const auto& dep : deps) {
        auto it = status_by_id.find(dep);
        if (it == status_by_id.end() || it->second != "done") {
          deps_ok = false;
          break;
        }
      }
      if (!deps_ok) continue;

      const int new_attempt = t.attempt + 1;
      if (!db_->claim_workflow_task(wf.workflow_id, t.task_id, now_unix_ms, new_attempt, &err)) {
        continue;
      }

      // Mark workflow as running once it has any running task.
      if (wf.status != "running") {
        wf.status = "running";
        wf.updated_unix_ms = now_unix_ms;
        (void)db_->upsert_workflow(wf, nullptr);
      }

      t.status = "running";
      t.attempt = new_attempt;
      t.started_unix_ms = now_unix_ms;
      t.updated_unix_ms = now_unix_ms;
      *out_wf = wf;
      *out_task = t;
      return true;
    }

    // No runnable tasks; check if the workflow is now terminal.
    maybe_finalize_workflow(wf.workflow_id);
  }

  return false;
}

void WorkflowEngine::execute_claimed_task(const AgentDb::WorkflowRow& wf, const AgentDb::WorkflowTaskRow& task) {
  const int64_t now = unix_ms_now();

  // Resolve template vars from completed tasks (assistant_text only).
  std::unordered_map<std::string, std::string> assistant_by_task;
  {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    std::string err;
    if (db_->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      for (const auto& t : tasks) {
        if (t.task_id.empty()) continue;
        if (t.status != "done") continue;
        if (t.result_json.empty()) continue;
        Json::Value r;
        std::string perr;
        if (!json_parse_any_value(t.result_json, &r, &perr) || !r.isObject()) continue;
        const std::string a = json_get_string(r, "assistant_text");
        if (!a.empty()) assistant_by_task[t.task_id] = a;
      }
    }
  }

  // Build final run request body, applying prompt templates.
  std::string request_body = task.request_json;
  Json::Value rr;
  std::string perr;
  if (json_parse_any_value(task.request_json, &rr, &perr) && rr.isObject()) {
    const std::string prompt = json_get_string(rr, "prompt");
    if (!prompt.empty()) {
      rr["prompt"] = expand_prompt_templates(prompt, assistant_by_task);
      request_body = json_stringify_compact(rr);
    }
  }

  const DaemonConfig cfg = cfg_snapshot_();
  const OpenAIClientConfig ocfg = ocfg_from_cfg_(cfg);
  Json::Value out = run_request_to_json_internal(cfg, ocfg, db_, tool_ext_or_null_, sessions_root_dir_, request_body, nullptr);

  std::string expect_err;
  const bool expect_ok = apply_expectations(task.expect_json, out, &expect_err);
  const bool run_ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();

  AgentDb::WorkflowTaskRow upd = task;
  upd.updated_unix_ms = now;
  upd.finished_unix_ms = now;
  upd.result_json = json_stringify_compact(out);

  if (run_ok && expect_ok) {
    upd.status = "done";
    upd.error.clear();
    upd.ready_unix_ms = 0;
    (void)db_->upsert_workflow_task(upd, nullptr);
    maybe_finalize_workflow(wf.workflow_id);
    return;
  }

  const bool can_retry = upd.attempt < std::max(1, upd.max_attempts);
  upd.error = !expect_ok ? expect_err : (out.isObject() && out.isMember("error") && out["error"].isString() ? out["error"].asString() : "error");

  if (can_retry && !wf.cancel_requested) {
    upd.status = "queued";
    upd.ready_unix_ms = now + retry_backoff_ms(upd.attempt);
    (void)db_->upsert_workflow_task(upd, nullptr);
    return;
  }

  upd.status = "error";
  upd.ready_unix_ms = 0;
  (void)db_->upsert_workflow_task(upd, nullptr);
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
  bool any_error = false;
  std::string first_error;
  bool all_terminal = true;
  bool all_done = true;
  for (const auto& t : tasks) {
    if (t.status == "running") any_running = true;
    if (t.status == "queued") any_queued = true;
    if (t.status == "error") {
      any_error = true;
      if (first_error.empty()) first_error = t.error;
    }
    const bool terminal = (t.status == "done" || t.status == "error" || t.status == "cancelled");
    if (!terminal) all_terminal = false;
    if (t.status != "done") all_done = false;
  }

  std::string new_status = wf.status;
  std::string wf_error = wf.error;

  if (any_error) {
    new_status = "error";
    if (wf_error.empty()) wf_error = first_error.empty() ? "workflow task failed" : first_error;
  } else if (wf.cancel_requested) {
    if (all_terminal) {
      new_status = "cancelled";
      if (wf_error.empty()) wf_error = "cancelled";
    } else {
      new_status = "running";
    }
  } else if (all_done && !tasks.empty()) {
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
    wf.status = new_status;
    wf.updated_unix_ms = unix_ms_now();
    wf.error = wf_error;
    if (!result_json.empty()) wf.result_json = result_json;
    (void)db_->upsert_workflow(wf, nullptr);
  }
}

}  // namespace agentd
