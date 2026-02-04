#include "edge_workflow_engine.h"

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

static std::string make_uuidish_msg_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);
  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);
  char buf[96];
  (void)snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
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

struct ToolMeta {
  std::string side_effect_level;
  std::unordered_set<std::string> hazards;
  bool has_rate_limit = false;
  int max_per_minute = 0;
  int cooldown_ms = 0;
};

static bool tool_meta_from_manifest(const Json::Value& manifest, const std::string& tool_name, ToolMeta* out_meta) {
  if (out_meta) *out_meta = ToolMeta{};
  if (!out_meta) return false;
  if (!manifest.isObject() || tool_name.empty()) return false;
  const auto& tools = manifest["tools"];
  if (!tools.isArray()) return false;
  for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
    const auto& t = tools[i];
    if (!t.isObject()) continue;
    if (!t.isMember("name") || !t["name"].isString()) continue;
    if (t["name"].asString() != tool_name) continue;
    ToolMeta m;
    if (t.isMember("side_effect_level") && t["side_effect_level"].isString()) m.side_effect_level = t["side_effect_level"].asString();
    if (t.isMember("hazards") && t["hazards"].isArray()) {
      for (Json::ArrayIndex j = 0; j < t["hazards"].size(); j++) {
        if (t["hazards"][j].isString()) m.hazards.insert(t["hazards"][j].asString());
      }
    }
    if (t.isMember("rate_limit") && t["rate_limit"].isObject()) {
      const auto& rl = t["rate_limit"];
      if (rl.isMember("max_per_minute") && rl["max_per_minute"].isInt()) m.max_per_minute = std::max(0, rl["max_per_minute"].asInt());
      else if (rl.isMember("max_per_minute") && rl["max_per_minute"].isUInt()) m.max_per_minute = (int)std::min((Json::UInt)INT32_MAX, rl["max_per_minute"].asUInt());
      if (rl.isMember("cooldown_ms") && rl["cooldown_ms"].isInt()) m.cooldown_ms = std::max(0, rl["cooldown_ms"].asInt());
      else if (rl.isMember("cooldown_ms") && rl["cooldown_ms"].isUInt()) m.cooldown_ms = (int)std::min((Json::UInt)INT32_MAX, rl["cooldown_ms"].asUInt());
      m.has_rate_limit = (m.max_per_minute > 0) || (m.cooldown_ms > 0);
    }
    *out_meta = std::move(m);
    return true;
  }
  return false;
}

static bool rate_limit_check_and_commit_best_effort(AgentDb* db, const std::string& node_id, const std::string& tool_name, const ToolMeta& meta) {
  if (!db || !db->is_open()) return false;
  if (!meta.has_rate_limit) return true;
  if (node_id.empty() || tool_name.empty()) return true;
  const int64_t now = unix_ms_now();

  AgentDb::EdgeToolRateStateRow st;
  std::string err;
  const bool has = db->get_edge_tool_rate_state(node_id, tool_name, &st, &err);
  if (!has) {
    st.node_id = node_id;
    st.tool_name = tool_name;
    st.window_start_utc_ms = 0;
    st.window_count = 0;
    st.last_call_utc_ms = 0;
  }
  if (meta.cooldown_ms > 0 && st.last_call_utc_ms > 0) {
    const int64_t since = now - st.last_call_utc_ms;
    if (since >= 0 && since < meta.cooldown_ms) return false;
  }
  const int64_t window_ms = 60000;
  if (st.window_start_utc_ms <= 0 || (now - st.window_start_utc_ms) >= window_ms || (now - st.window_start_utc_ms) < 0) {
    st.window_start_utc_ms = now;
    st.window_count = 0;
  }
  if (meta.max_per_minute > 0 && st.window_count >= meta.max_per_minute) return false;
  st.window_count += 1;
  st.last_call_utc_ms = now;
  (void)db->upsert_edge_tool_rate_state(st, nullptr);
  return true;
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

      std::vector<AgentDb::EdgeWorkflowStepRow> steps;
      std::string serr;
      if (!db_->list_edge_workflow_steps(wf.workflow_id, &steps, &serr)) continue;

      std::unordered_map<std::string, AgentDb::EdgeWorkflowStepRow> step_map;
      step_map.reserve(steps.size());
      for (const auto& s : steps) step_map[s.step_id] = s;

      // Recover step state from existing edge_tasks (durability after restart).
      for (auto& s : steps) {
        if (s.kind == "join") continue;
        if (s.state != "PENDING") continue;
        AgentDb::EdgeTaskRow tr;
        std::string terr;
        if (db_->get_edge_task(wf.workflow_id, s.step_id, &tr, &terr) && !tr.state.empty()) {
          s.state = tr.state;
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
        }
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

        // Safety/rate checks for invoke_tool.
        std::string tool_name;
        if (mode == "invoke") {
          tool_name = payload.isMember("tool") && payload["tool"].isString() ? trim_copy(payload["tool"].asString()) : "";
          if (tool_name.empty()) {
            s.state = "FAILED";
            s.error = "missing payload.tool";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          AgentDb::EdgeNodeRow nr;
          std::string nerr;
          if (!db_->get_edge_node(node_id, &nr, &nerr) || nr.manifest_json.empty()) {
            s.state = "FAILED";
            s.error = "node has no manifest";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          Json::Value manifest;
          if (!json_parse_any(nr.manifest_json, &manifest, &perr) || !manifest.isObject()) {
            s.state = "FAILED";
            s.error = "stored manifest parse failed";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          ToolMeta meta;
          if (!tool_meta_from_manifest(manifest, tool_name, &meta)) {
            s.state = "FAILED";
            s.error = "tool not found in manifest";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          // Platform policy: deny privacy_camera by default, and deny high side-effects for unattended workflows.
          if (meta.hazards.count("privacy_camera")) {
            s.state = "FAILED";
            s.error = "denied: hazard privacy_camera";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          if (meta.side_effect_level == "high") {
            s.state = "FAILED";
            s.error = "denied: side_effect_level high";
            s.updated_utc_ms = unix_ms_now();
            (void)db_->upsert_edge_workflow_step(s, nullptr);
            step_map[s.step_id] = s;
            continue;
          }
          if (!rate_limit_check_and_commit_best_effort(db_, node_id, tool_name, meta)) {
            // Best-effort: keep pending; try again later rather than failing the workflow permanently.
            continue;
          }
        }

        // Dispatch: create edge task and enqueue TASK_ASSIGN.
        const std::string idempotency_key = std::string("wf:") + wf.workflow_id + ":" + s.step_id;

        AgentDb::EdgeTaskRow tr;
        tr.task_id = wf.workflow_id;
        tr.step_id = s.step_id;
        tr.node_id = node_id;
        tr.idempotency_key = idempotency_key;
        tr.mode = mode;
        tr.tool_name = tool_name;
        tr.deadline_utc_ms = deadline_utc_ms;
        tr.payload_json = json_stringify_compact(payload);
        tr.state = "QUEUED";
        tr.created_utc_ms = unix_ms_now();
        tr.updated_utc_ms = tr.created_utc_ms;
        std::string terr;
        if (!db_->upsert_edge_task(tr, &terr)) {
          s.state = "FAILED";
          s.error = std::string("persist task failed: ") + terr;
          s.updated_utc_ms = unix_ms_now();
          (void)db_->upsert_edge_workflow_step(s, nullptr);
          step_map[s.step_id] = s;
          continue;
        }

        AgentDb::EdgeTaskEventRow tev;
        tev.task_id = tr.task_id;
        tev.step_id = tr.step_id;
        tev.ts_utc_ms = tr.created_utc_ms;
        tev.state = "QUEUED";
        Json::Value td(Json::objectValue);
        td["task_id"] = tr.task_id;
        td["step_id"] = tr.step_id;
        td["node_id"] = node_id;
        td["state"] = "QUEUED";
        td["mode"] = mode;
        td["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
        if (!tool_name.empty()) td["tool"] = tool_name;
        tev.data_json = json_stringify_compact(td);
        (void)db_->insert_edge_task_event(tev, nullptr, nullptr);

        Json::Value env(Json::objectValue);
        env["msg_id"] = make_uuidish_msg_id();
        env["ts_utc_ms"] = (Json::Int64)unix_ms_now();
        env["type"] = "TASK_ASSIGN";
        env["from"] = "platform";
        env["to"] = std::string("node:") + node_id;
        Json::Value body(Json::objectValue);
        body["task_id"] = wf.workflow_id;
        body["step_id"] = s.step_id;
        body["idempotency_key"] = idempotency_key;
        body["mode"] = mode;
        body["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
        body["payload"] = payload;
        env["body"] = body;
        AgentDb::EdgeOutboxMessageRow orow;
        orow.node_id = node_id;
        orow.ts_utc_ms = unix_ms_now();
        orow.envelope_json = json_stringify_compact(env);
        int64_t outbox_id = 0;
        if (!db_->insert_edge_outbox_message(orow, &outbox_id, &terr)) {
          // Leave step pending so we can retry dispatch later.
          continue;
        }

        s.state = "QUEUED";
        s.updated_utc_ms = unix_ms_now();
        (void)db_->upsert_edge_workflow_step(s, nullptr);
        step_map[s.step_id] = s;

        wf.status = "RUNNING";
        wf.updated_utc_ms = unix_ms_now();
        (void)db_->upsert_edge_workflow(wf, nullptr);

        dispatch_budget--;
      }

      // Update workflow status based on current steps.
      std::string next_status;
      std::string next_err;
      compute_workflow_status(steps, &next_status, &next_err);
      if (!next_status.empty() && (wf.status != next_status || (next_status == "FAILED" && wf.error != next_err))) {
        wf.status = next_status;
        wf.updated_utc_ms = unix_ms_now();
        if (next_status == "FAILED" && !next_err.empty()) wf.error = next_err;
        (void)db_->upsert_edge_workflow(wf, nullptr);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
}

}  // namespace agentd
