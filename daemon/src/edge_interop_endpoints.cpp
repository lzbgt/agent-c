#include "edge_interop_endpoints.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <random>
#include <sstream>
#include <string>
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

static bool parse_string_array(const std::string& json, std::unordered_set<std::string>* out) {
  if (!out) return false;
  out->clear();
  if (json.empty()) return true;
  Json::Value v;
  std::string err;
  if (!json_parse_any(json, &v, &err) || !v.isArray()) return false;
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (v[i].isString()) out->insert(v[i].asString());
  }
  return true;
}

static void manifest_extract_best_effort(
  const Json::Value& manifest,
  std::string* out_tags_json,
  std::string* out_tools_json,
  std::string* out_hw_presence_json
) {
  if (out_tags_json) *out_tags_json = "[]";
  if (out_tools_json) *out_tools_json = "[]";
  if (out_hw_presence_json) *out_hw_presence_json = "{}";
  if (!manifest.isObject()) return;

  if (out_tags_json) {
    const auto& tags = manifest["tags"];
    if (tags.isArray()) *out_tags_json = json_stringify_compact(tags);
  }

  if (out_tools_json) {
    Json::Value arr(Json::arrayValue);
    const auto& tools = manifest["tools"];
    if (tools.isArray()) {
      for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
        const auto& t = tools[i];
        if (!t.isObject()) continue;
        const auto& n = t["name"];
        if (n.isString()) arr.append(n.asString());
      }
    }
    *out_tools_json = json_stringify_compact(arr);
  }

  if (out_hw_presence_json) {
    const auto& hw = manifest["hardware"];
    if (hw.isObject()) {
      const auto& pres = hw["presence"];
      if (pres.isObject()) *out_hw_presence_json = json_stringify_compact(pres);
    }
  }
}

static std::string node_to_prefix(const std::string& node_id) {
  if (node_id.empty()) return "";
  if (node_id.rfind("node:", 0) == 0) return node_id;
  return std::string("node:") + node_id;
}

static bool is_terminal_edge_task_state(const std::string& s) {
  return s == "SUCCEEDED" || s == "FAILED" || s == "TIMED_OUT" || s == "CANCELED";
}

struct ToolMeta {
  std::string side_effect_level;
  std::unordered_set<std::string> hazards;
  bool has_rate_limit = false;
  int max_per_minute = 0;
  int cooldown_ms = 0;
};

static bool tool_meta_from_manifest(
  const Json::Value& manifest,
  const std::string& tool_name,
  ToolMeta* out_meta,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_meta) *out_meta = ToolMeta{};
  if (!out_meta) return false;
  if (!manifest.isObject()) {
    if (out_error) *out_error = "manifest is not an object";
    return false;
  }
  if (tool_name.empty()) {
    if (out_error) *out_error = "tool_name is empty";
    return false;
  }
  const auto& tools = manifest["tools"];
  if (!tools.isArray()) {
    if (out_error) *out_error = "manifest.tools missing/invalid";
    return false;
  }
  for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
    const auto& t = tools[i];
    if (!t.isObject()) continue;
    const auto& n = t["name"];
    if (!n.isString()) continue;
    if (n.asString() != tool_name) continue;

    ToolMeta m;
    if (t.isMember("side_effect_level") && t["side_effect_level"].isString()) {
      m.side_effect_level = t["side_effect_level"].asString();
    }
    if (t.isMember("hazards") && t["hazards"].isArray()) {
      for (Json::ArrayIndex j = 0; j < t["hazards"].size(); j++) {
        if (t["hazards"][j].isString()) m.hazards.insert(t["hazards"][j].asString());
      }
    }
    if (t.isMember("rate_limit") && t["rate_limit"].isObject()) {
      const auto& rl = t["rate_limit"];
      if (rl.isMember("max_per_minute") && rl["max_per_minute"].isInt()) {
        m.max_per_minute = std::max(0, rl["max_per_minute"].asInt());
      } else if (rl.isMember("max_per_minute") && rl["max_per_minute"].isUInt()) {
        m.max_per_minute = (int)std::min((Json::UInt)INT32_MAX, rl["max_per_minute"].asUInt());
      }
      if (rl.isMember("cooldown_ms") && rl["cooldown_ms"].isInt()) {
        m.cooldown_ms = std::max(0, rl["cooldown_ms"].asInt());
      } else if (rl.isMember("cooldown_ms") && rl["cooldown_ms"].isUInt()) {
        m.cooldown_ms = (int)std::min((Json::UInt)INT32_MAX, rl["cooldown_ms"].asUInt());
      }
      m.has_rate_limit = (m.max_per_minute > 0) || (m.cooldown_ms > 0);
    }
    *out_meta = std::move(m);
    return true;
  }
  if (out_error) *out_error = "tool not found in manifest";
  return false;
}

static bool edge_rate_limit_check_best_effort(
  AgentDb* db,
  const std::string& node_id,
  const std::string& tool_name,
  const ToolMeta& meta,
  int64_t now_utc_ms,
  AgentDb::EdgeToolRateStateRow* out_next_state,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_next_state) *out_next_state = AgentDb::EdgeToolRateStateRow{};
  if (!db || !db->is_open()) return false;
  if (!meta.has_rate_limit) return true;
  if (node_id.empty() || tool_name.empty()) return true;
  const int64_t now = now_utc_ms > 0 ? now_utc_ms : unix_ms_now();

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
    if (since >= 0 && since < meta.cooldown_ms) {
      if (out_error) *out_error = "rate_limited: cooldown";
      return false;
    }
  }

  const int64_t window_ms = 60000;
  if (st.window_start_utc_ms <= 0 || (now - st.window_start_utc_ms) >= window_ms || (now - st.window_start_utc_ms) < 0) {
    st.window_start_utc_ms = now;
    st.window_count = 0;
  }
  if (meta.max_per_minute > 0 && st.window_count >= meta.max_per_minute) {
    if (out_error) *out_error = "rate_limited: max_per_minute";
    return false;
  }

  st.window_count += 1;
  st.last_call_utc_ms = now;
  if (out_next_state) *out_next_state = st;
  return true;
}

static void edge_rate_limit_commit_best_effort(
  AgentDb* db,
  const AgentDb::EdgeToolRateStateRow& next_state
) {
  if (!db || !db->is_open()) return;
  if (next_state.node_id.empty() || next_state.tool_name.empty()) return;
  (void)db->upsert_edge_tool_rate_state(next_state, nullptr);
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
    if (!parse_string_array(n.tools_json, &toolset)) {
      // If node's tools list can't be parsed, treat as unknown => skip for strict matching.
      continue;
    }
    if (!parse_string_array(n.tags_json, &tagset)) {
      continue;
    }

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

static bool edge_enqueue_task_assign(
  AgentDb* db,
  const std::string& node_id,
  const std::string& task_id,
  const std::string& step_id,
  const std::string& idempotency_key,
  const std::string& mode,
  int64_t deadline_utc_ms,
  const Json::Value& payload,
  const std::unordered_set<std::string>& allow_hazards,
  bool allow_high_side_effect,
  bool enforce_safety,
  bool enforce_rate_limit,
  int64_t* out_outbox_id,
  bool* out_deduped,
  std::string* out_error,
  int* out_http_status
) {
  if (out_error) out_error->clear();
  if (out_http_status) *out_http_status = 500;
  if (out_outbox_id) *out_outbox_id = 0;
  if (out_deduped) *out_deduped = false;
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    if (out_http_status) *out_http_status = 503;
    return false;
  }
  if (node_id.empty() || !id_is_safe(node_id)) {
    if (out_error) *out_error = "missing/invalid node_id";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    if (out_error) *out_error = "missing/invalid task fields";
    if (out_http_status) *out_http_status = 400;
    return false;
  }

  std::string tool_name;
  if (mode == "invoke") {
    tool_name = payload.isMember("tool") && payload["tool"].isString() ? trim_copy(payload["tool"].asString()) : "";
    if (tool_name.empty()) {
      if (out_error) *out_error = "missing payload.tool for mode=invoke";
      if (out_http_status) *out_http_status = 400;
      return false;
    }
  }

  bool have_rate_state_to_commit = false;
  AgentDb::EdgeToolRateStateRow next_rate_state;

  // Dedupe on (node_id, idempotency_key).
  AgentDb::EdgeTaskRow existing;
  std::string err;
  const bool has_existing = db->get_edge_task_by_node_idempotency(node_id, idempotency_key, &existing, &err);
  if (has_existing) {
    if (out_deduped) *out_deduped = true;
    if (existing.task_id != task_id || existing.step_id != step_id) {
      if (out_error) *out_error = "idempotency_key collision with different task_id/step_id";
      if (out_http_status) *out_http_status = 409;
      return false;
    }
  } else {
    // UM‑SAFE best-effort guardrails for new work:
    // - require manifest for mode=invoke (so we can inspect hazards/rate_limit)
    // - deny privacy_camera by default (unless explicitly allowed)
    // - require explicit opt-in for side_effect_level="high"
    // - enforce tool rate_limit using platform-side state
    if (mode == "invoke" && (enforce_safety || enforce_rate_limit)) {
      AgentDb::EdgeNodeRow nr;
      std::string nerr;
      if (!db->get_edge_node(node_id, &nr, &nerr)) {
        if (out_error) *out_error = "node not found";
        if (out_http_status) *out_http_status = 404;
        return false;
      }
      if (nr.manifest_json.empty()) {
        if (out_error) *out_error = "node has no manifest (caps unknown)";
        if (out_http_status) *out_http_status = 409;
        return false;
      }

      Json::Value manifest;
      std::string merr;
      if (!json_parse_any(nr.manifest_json, &manifest, &merr) || !manifest.isObject()) {
        if (out_error) *out_error = "stored manifest parse failed";
        if (out_http_status) *out_http_status = 500;
        return false;
      }

      ToolMeta meta;
      std::string terr;
      if (!tool_meta_from_manifest(manifest, tool_name, &meta, &terr)) {
        if (out_error) *out_error = std::string("tool not present in node manifest: ") + terr;
        if (out_http_status) *out_http_status = 409;
        return false;
      }

      if (enforce_safety) {
        // Minimal default denylist per handoff doc.
        if (meta.hazards.count("privacy_camera") && !allow_hazards.count("privacy_camera")) {
          if (out_error) *out_error = "denied: hazard privacy_camera";
          if (out_http_status) *out_http_status = 403;
          return false;
        }

        if (meta.side_effect_level == "high" && !allow_high_side_effect) {
          if (out_error) *out_error = "denied: side_effect_level high (set allow_high_side_effect=true to override)";
          if (out_http_status) *out_http_status = 403;
          return false;
        }
      }

      if (enforce_rate_limit) {
        std::string rlerr;
        if (!edge_rate_limit_check_best_effort(db, node_id, tool_name, meta, unix_ms_now(), &next_rate_state, &rlerr)) {
          if (out_error) *out_error = rlerr.empty() ? "rate_limited" : rlerr;
          if (out_http_status) *out_http_status = 429;
          return false;
        }
        if (meta.has_rate_limit) have_rate_state_to_commit = true;
      }
    }

    AgentDb::EdgeTaskRow tr;
    tr.task_id = task_id;
    tr.step_id = step_id;
    tr.node_id = node_id;
    tr.idempotency_key = idempotency_key;
    tr.mode = mode;
    tr.tool_name = tool_name;
    tr.deadline_utc_ms = deadline_utc_ms;
    tr.payload_json = json_stringify_compact(payload);
    tr.state = "QUEUED";
    tr.created_utc_ms = unix_ms_now();
    tr.updated_utc_ms = tr.created_utc_ms;
    if (!db->upsert_edge_task(tr, &err)) {
      if (out_error) *out_error = std::string("failed to persist edge task: ") + err;
      if (out_http_status) *out_http_status = 500;
      return false;
    }

    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = tr.created_utc_ms;
    ev.state = "QUEUED";
    Json::Value d(Json::objectValue);
    d["task_id"] = task_id;
    d["step_id"] = step_id;
    d["node_id"] = node_id;
    d["state"] = "QUEUED";
    d["mode"] = mode;
    d["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
    if (!tool_name.empty()) d["tool"] = tool_name;
    ev.data_json = json_stringify_compact(d);
    (void)db->insert_edge_task_event(ev, nullptr, nullptr);
  }

  // Enqueue UM‑BMP TASK_ASSIGN envelope to node outbox.
  const int64_t now = unix_ms_now();
  Json::Value env(Json::objectValue);
  env["msg_id"] = make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)now;
  env["type"] = "TASK_ASSIGN";
  env["from"] = "platform";
  env["to"] = node_to_prefix(node_id);
  Json::Value b(Json::objectValue);
  b["task_id"] = task_id;
  b["step_id"] = step_id;
  b["idempotency_key"] = idempotency_key;
  b["mode"] = mode;
  b["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
  b["payload"] = payload;
  env["body"] = b;

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = node_id;
  orow.ts_utc_ms = now;
  orow.envelope_json = json_stringify_compact(env);
  int64_t outbox_id = 0;
  if (!db->insert_edge_outbox_message(orow, &outbox_id, &err)) {
    if (out_error) *out_error = std::string("failed to enqueue outbox message: ") + err;
    if (out_http_status) *out_http_status = 500;
    return false;
  }

  if (out_outbox_id) *out_outbox_id = outbox_id;

  // Best-effort commit of any rate-limit state (if applicable) after successful enqueue.
  if (!has_existing && enforce_rate_limit && have_rate_state_to_commit) {
    edge_rate_limit_commit_best_effort(db, next_rate_state);
  }

  if (out_http_status) *out_http_status = 200;
  return true;
}

static void edge_apply_rules_for_sensor_event_best_effort(
  AgentDb* db,
  const std::string& sensor_node_id,
  const std::string& sensor_msg_id,
  const std::string& event_type,
  int64_t event_ts_utc_ms,
  double confidence,
  const Json::Value& data
) {
  if (!db || !db->is_open()) return;
  if (event_type.empty()) return;

  const int64_t now = unix_ms_now();
  std::vector<AgentDb::EdgeRuleRow> rules;
  std::string err;
  if (!db->list_edge_rules(/*max_rows=*/256, &rules, &err)) return;

  for (auto r : rules) {
    if (!r.enabled) continue;
    if (r.event_type != event_type) continue;
    if (confidence < r.min_confidence) continue;
    if (r.cooldown_ms > 0 && r.last_fired_utc_ms > 0) {
      const int64_t since = now - r.last_fired_utc_ms;
      if (since >= 0 && since < (int64_t)r.cooldown_ms) continue;
    }

    Json::Value action;
    std::string perr;
    if (!json_parse_any(r.action_json, &action, &perr) || !action.isObject()) continue;
    const std::string type = action.isMember("type") && action["type"].isString() ? action["type"].asString() : "";
    if (type != "task_assign") continue;

    // Target selection.
    std::string target_node_id;
    if (action.isMember("target") && action["target"].isObject()) {
      const auto& tgt = action["target"];
      if (tgt.isMember("node_id") && tgt["node_id"].isString()) {
        target_node_id = trim_copy(tgt["node_id"].asString());
      } else if (tgt.isMember("match_any") && tgt["match_any"].isObject()) {
        std::vector<std::string> requires_tools, tags_all, tags_any, tags_none;
        const auto& m = tgt["match_any"];
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
        (void)select_node_match_any(db, requires_tools, tags_all, tags_any, tags_none, &target_node_id);
      }
    }
    if (target_node_id.empty()) {
      // Safe default: if rule doesn't specify a target, use the sensor node itself.
      target_node_id = sensor_node_id;
    }

    const std::string mode = action.isMember("mode") && action["mode"].isString() ? action["mode"].asString() : "invoke";
    Json::Value payload = action.isMember("payload") ? action["payload"] : Json::Value(Json::nullValue);
    if (!payload.isObject()) continue;

    int64_t deadline_in_ms = 60000;
    if (action.isMember("deadline_in_ms") && (action["deadline_in_ms"].isInt64() || action["deadline_in_ms"].isUInt64())) {
      deadline_in_ms = action["deadline_in_ms"].isInt64() ? action["deadline_in_ms"].asInt64() : (int64_t)action["deadline_in_ms"].asUInt64();
    }
    if (deadline_in_ms < 1000) deadline_in_ms = 1000;
    if (deadline_in_ms > 24LL * 60 * 60 * 1000) deadline_in_ms = 24LL * 60 * 60 * 1000;
    const int64_t deadline_utc_ms = now + deadline_in_ms;

    std::string task_id = action.isMember("task_id") && action["task_id"].isString() ? action["task_id"].asString() : "";
    if (task_id.empty()) task_id = std::string("rule_") + make_uuidish_msg_id();
    std::string step_id = action.isMember("step_id") && action["step_id"].isString() ? action["step_id"].asString() : "auto";
    std::string idempotency_key = action.isMember("idempotency_key") && action["idempotency_key"].isString() ? action["idempotency_key"].asString() : "";
    if (idempotency_key.empty()) {
      idempotency_key = std::string("rule:") + r.rule_id + ":msg:" + sensor_msg_id;
    }

    // Automation defaults: do NOT allow hazards or high side-effects by default.
    std::unordered_set<std::string> allow_hazards;
    const bool allow_high_side_effect = false;

    int64_t outbox_id = 0;
    bool deduped = false;
    std::string derr;
    int http = 0;
    if (!edge_enqueue_task_assign(
          db,
          target_node_id,
          task_id,
          step_id,
          idempotency_key,
          mode,
          deadline_utc_ms,
          payload,
          allow_hazards,
          allow_high_side_effect,
          /*enforce_safety=*/true,
          /*enforce_rate_limit=*/true,
          &outbox_id,
          &deduped,
          &derr,
          &http)) {
      continue;
    }

    // Mark rule fired (best-effort).
    r.last_fired_utc_ms = now;
    r.updated_utc_ms = now;
    (void)db->upsert_edge_rule(r, nullptr);

    // Optional: attach light provenance into the task payload for downstream debugging.
    (void)event_ts_utc_ms;
    (void)data;
  }
}

}  // namespace

void handle_edge_message_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value env;
  std::string perr;
  if (!json_parse_object(req.body, &env, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  const std::string msg_id = env.isMember("msg_id") && env["msg_id"].isString() ? trim_copy(env["msg_id"].asString()) : "";
  const int64_t ts_utc_ms = env.isMember("ts_utc_ms") && env["ts_utc_ms"].isInt64() ? env["ts_utc_ms"].asInt64()
    : (env.isMember("ts_utc_ms") && env["ts_utc_ms"].isUInt64() ? (int64_t)env["ts_utc_ms"].asUInt64() : 0);
  const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
  const std::string from_id = env.isMember("from") && env["from"].isString() ? trim_copy(env["from"].asString()) : "";
  std::string to_id;
  if (env.isMember("to")) {
    if (env["to"].isString()) to_id = trim_copy(env["to"].asString());
    else if (env["to"].isNull()) to_id.clear();
    else {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid envelope.to (expected string|null)\"}";
      return;
    }
  }
  const Json::Value body = env.isMember("body") ? env["body"] : Json::Value(Json::nullValue);

  if (msg_id.empty() || type.empty() || !body.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope (missing msg_id/type/body)\"}";
    return;
  }
  if (!id_is_safe(type) || msg_id.size() > 128) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope msg_id/type\"}";
    return;
  }

  // Persist inbound (dedupe by msg_id).
  {
    AgentDb::EdgeInboxMessageRow ir;
    ir.msg_id = msg_id;
    ir.ts_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : unix_ms_now();
    ir.type = type;
    ir.from_id = from_id;
    ir.to_id = to_id;
    ir.envelope_json = json_stringify_compact(env);
    std::string derr;
    if (!db_or_null->insert_edge_inbox_message(ir, &derr)) {
      // Treat duplicates as OK (idempotent ingest).
      if (derr.find("UNIQUE") != std::string::npos || derr.find("constraint") != std::string::npos) {
        resp->body = "{\"ok\":true,\"deduped\":true}";
        return;
      }
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist inbound message";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  const int64_t now = unix_ms_now();
  auto queue_caps_req = [&](const std::string& node_id) {
    if (node_id.empty()) return;
    Json::Value oenv(Json::objectValue);
    oenv["msg_id"] = make_uuidish_msg_id();
    oenv["ts_utc_ms"] = (Json::Int64)now;
    oenv["type"] = "PLATFORM_CAPS_REQ";
    oenv["from"] = "platform";
    oenv["to"] = node_to_prefix(node_id);
    Json::Value b(Json::objectValue);
    b["node_id"] = node_id;
    b["want"] = "full";
    oenv["body"] = b;
    AgentDb::EdgeOutboxMessageRow orow;
    orow.node_id = node_id;
    orow.ts_utc_ms = now;
    orow.envelope_json = json_stringify_compact(oenv);
    (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
  };

  if (type == "NODE_HELLO") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string model = body.isMember("model") && body["model"].isString() ? body["model"].asString() : "";
    const std::string fw_git_sha = body.isMember("fw_git_sha") && body["fw_git_sha"].isString() ? body["fw_git_sha"].asString() : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    if (node_id.empty() || !id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid node_id\"}";
      return;
    }

    AgentDb::EdgeNodeRow prev;
    std::string gerr;
    const bool have = db_or_null->get_edge_node(node_id, &prev, &gerr);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.model = model;
    nr.fw_git_sha = fw_git_sha;
    nr.caps_sha256 = caps_sha;
    nr.last_hello_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    (void)db_or_null->upsert_edge_node(nr, &uerr);

    const bool need_caps =
      !have || prev.manifest_json.empty() || (!caps_sha.empty() && prev.caps_sha256 != caps_sha);
    if (need_caps) queue_caps_req(node_id);

    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_HEARTBEAT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    const Json::Value health = body.isMember("health") ? body["health"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid node_id\"}";
      return;
    }

    AgentDb::EdgeNodeRow prev;
    std::string gerr;
    const bool have = db_or_null->get_edge_node(node_id, &prev, &gerr);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.caps_sha256 = caps_sha;
    if (health.isObject()) nr.health_json = json_stringify_compact(health);
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    (void)db_or_null->upsert_edge_node(nr, &uerr);

    const bool need_caps =
      !have || prev.manifest_json.empty() || (!caps_sha.empty() && prev.caps_sha256 != caps_sha);
    if (need_caps) queue_caps_req(node_id);

    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_CAPS_RSP") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const Json::Value manifest = body.isMember("manifest") ? body["manifest"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !id_is_safe(node_id) || !manifest.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid NODE_CAPS_RSP body\"}";
      return;
    }

    std::string tags_json, tools_json, hw_json;
    manifest_extract_best_effort(manifest, &tags_json, &tools_json, &hw_json);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    if (manifest.isMember("caps_sha256") && manifest["caps_sha256"].isString()) {
      nr.caps_sha256 = manifest["caps_sha256"].asString();
    }
    nr.manifest_json = json_stringify_compact(manifest);
    nr.tags_json = tags_json;
    nr.tools_json = tools_json;
    nr.hardware_presence_json = hw_json;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node manifest";
      o["detail"] = uerr;
      resp->body = json_stringify_compact(o);
      return;
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  auto update_task_state = [&](const std::string& task_id, const std::string& step_id, const std::string& state,
                               const std::string& result_json, const std::string& error_text, const Json::Value& event_data) {
    if (task_id.empty() || step_id.empty() || state.empty()) return;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (!db_or_null->get_edge_task(task_id, step_id, &tr, &terr)) {
      // Unknown task; ignore to keep ingestion robust.
      return;
    }
    const bool terminal = is_terminal_edge_task_state(tr.state);
    const int64_t ts_eff = ts_utc_ms > 0 ? ts_utc_ms : now;

    // Do not regress/override terminal states set by the platform (e.g. deadline sweeper) or earlier completion.
    // We still persist the event for observability.
    const bool apply_update = !terminal;
    const std::string effective_state = apply_update ? state : tr.state;
    if (apply_update) {
      tr.state = state;
      tr.updated_utc_ms = ts_eff;
      if (!result_json.empty()) tr.result_json = result_json;
      if (!error_text.empty()) tr.error = error_text;
      (void)db_or_null->upsert_edge_task(tr, nullptr);
    }
    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = ts_eff;
    ev.state = state;
    Json::Value d = event_data.isNull() ? Json::Value(Json::objectValue) : event_data;
    if (terminal && state != tr.state) {
      d["_ignored_by_platform"] = true;
      d["_platform_state"] = tr.state;
    }
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_edge_task_event(ev, nullptr, nullptr);

    // Best-effort: if this task belongs to an edge workflow (task_id == workflow_id), reflect state into the step.
    if (apply_update || state == effective_state) {
      AgentDb::EdgeWorkflowRow wf;
      std::string werr;
      if (db_or_null->get_edge_workflow(task_id, &wf, &werr)) {
        std::vector<AgentDb::EdgeWorkflowStepRow> steps;
        std::string serr;
        if (db_or_null->list_edge_workflow_steps(task_id, &steps, &serr)) {
          for (auto& s : steps) {
            if (s.step_id != step_id) continue;
            if (s.state != effective_state) {
              s.state = effective_state;
              s.updated_utc_ms = ts_eff;
              if (!error_text.empty()) s.error = error_text;
              (void)db_or_null->upsert_edge_workflow_step(s, nullptr);
            }
            break;
          }
        }
      }
    }
  };

  if (type == "TASK_ACK") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const bool accepted = body.isMember("accepted") && body["accepted"].isBool() ? body["accepted"].asBool() : false;
    std::string reason;
    if (body.isMember("reason") && body["reason"].isString()) reason = body["reason"].asString();
    Json::Value d = body;
    if (accepted) {
      update_task_state(task_id, step_id, "QUEUED", /*result_json=*/"", /*error_text=*/"", d);
    } else {
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", reason.empty() ? "rejected" : reason, d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_EVENT") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string state = body.isMember("state") && body["state"].isString() ? body["state"].asString() : "";
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    std::string result_json;
    if (body.isMember("result")) result_json = json_stringify_compact(body["result"]);
    update_task_state(task_id, step_id, state, result_json, error, body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_DONE") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    std::string result_json;
    if (body.isMember("result")) result_json = json_stringify_compact(body["result"]);
    update_task_state(task_id, step_id, "SUCCEEDED", result_json, /*error_text=*/"", body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_FAILED") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", error.empty() ? "failed" : error, body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "SENSOR_EVENT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string event_type = body.isMember("event_type") && body["event_type"].isString() ? body["event_type"].asString() : "";
    const int64_t ts2 = body.isMember("ts_utc_ms") && (body["ts_utc_ms"].isInt64() || body["ts_utc_ms"].isUInt64())
      ? (body["ts_utc_ms"].isInt64() ? body["ts_utc_ms"].asInt64() : (int64_t)body["ts_utc_ms"].asUInt64())
      : (ts_utc_ms > 0 ? ts_utc_ms : now);
    const double confidence = body.isMember("confidence") && (body["confidence"].isDouble() || body["confidence"].isInt())
      ? body["confidence"].asDouble()
      : 0.0;
    Json::Value data = body.isMember("data") ? body["data"] : Json::Value(Json::objectValue);
    if (node_id.empty() || !id_is_safe(node_id) || event_type.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid SENSOR_EVENT body\"}";
      return;
    }
    AgentDb::EdgeSensorEventRow sr;
    sr.node_id = node_id;
    sr.event_type = event_type;
    sr.ts_utc_ms = ts2;
    sr.confidence = confidence;
    sr.data_json = json_stringify_compact(data.isNull() ? Json::Value(Json::objectValue) : data);
    (void)db_or_null->insert_edge_sensor_event(sr, nullptr, nullptr);

    // Best-effort automation bridge: apply enabled rules for this event.
    edge_apply_rules_for_sensor_event_best_effort(
      db_or_null,
      node_id,
      msg_id,
      event_type,
      ts2,
      confidence,
      data
    );
    resp->body = "{\"ok\":true}";
    return;
  }

  // Unknown message types are accepted and persisted (forward compatible).
  resp->body = "{\"ok\":true}";
}

void handle_edge_outbox_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  int64_t cursor = 0;
  const auto c = query_get(req.query, "cursor");
  if (c && !c->empty()) {
    try { cursor = (int64_t)std::stoll(*c); } catch (...) { cursor = 0; }
  }
  if (cursor < 0) cursor = 0;

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 2048));

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  std::string err;
  if (!db_or_null->list_edge_outbox_messages(*nid, cursor, limit, &msgs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list outbox";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& m : msgs) {
    Json::Value row(Json::objectValue);
    row["outbox_id"] = (Json::Int64)m.outbox_id;
    row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
    Json::Value env;
    std::string perr;
    if (json_parse_any(m.envelope_json, &env, &perr) && env.isObject()) {
      row["msg"] = env;
    } else {
      row["msg_raw"] = m.envelope_json;
      row["parse_error"] = perr;
    }
    arr.append(row);
    cursor_next = std::max(cursor_next, m.outbox_id);
  }
  o["messages"] = arr;
  o["cursor_next"] = (Json::Int64)cursor_next;
  resp->body = json_stringify_compact(o);
}

void handle_edge_nodes_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db_or_null->list_edge_nodes(limit, &nodes, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list nodes";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  for (const auto& n : nodes) {
    Json::Value row(Json::objectValue);
    row["node_id"] = n.node_id;
    if (!n.model.empty()) row["model"] = n.model;
    if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
    if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
    row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
    row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
    arr.append(row);
  }
  o["nodes"] = arr;
  resp->body = json_stringify_compact(o);
}

void handle_edge_node_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value row(Json::objectValue);
  row["node_id"] = n.node_id;
  if (!n.model.empty()) row["model"] = n.model;
  if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
  if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
  row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
  row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
  if (!n.tags_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.tags_json, &v, &perr) && v.isArray()) row["tags"] = v;
  }
  if (!n.tools_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.tools_json, &v, &perr) && v.isArray()) row["tools"] = v;
  }
  if (!n.hardware_presence_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.hardware_presence_json, &v, &perr) && v.isObject()) row["hardware_presence"] = v;
  }
  if (!n.health_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.health_json, &v, &perr) && v.isObject()) row["health"] = v;
  }
  row["has_manifest"] = !n.manifest_json.empty();
  o["node"] = row;
  resp->body = json_stringify_compact(o);
}

void handle_edge_node_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }
  if (n.manifest_json.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node has no manifest\"}";
    return;
  }

  Json::Value m;
  std::string perr;
  if (!json_parse_any(n.manifest_json, &m, &perr) || !m.isObject()) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to parse stored manifest";
    o["parse_error"] = perr;
    resp->body = json_stringify_compact(o);
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["manifest"] = m;
  resp->body = json_stringify_compact(o);
}

void handle_edge_task_assign_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  std::string node_id = args.isMember("node_id") && args["node_id"].isString() ? trim_copy(args["node_id"].asString()) : "";
  std::vector<std::string> requires_tools;
  std::vector<std::string> tags_all;
  std::vector<std::string> tags_any;
  std::vector<std::string> tags_none;
  if (node_id.empty() && args.isMember("match_any") && args["match_any"].isObject()) {
    const auto& m = args["match_any"];
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

    if (!select_node_match_any(db_or_null, requires_tools, tags_all, tags_any, tags_none, &node_id)) {
      resp->status = 409;
      resp->body = "{\"ok\":false,\"error\":\"no matching node\"}";
      return;
    }
  }
  if (node_id.empty() || !id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id (or match_any did not select)\"}";
    return;
  }

  const std::string task_id = args.isMember("task_id") && args["task_id"].isString() ? args["task_id"].asString() : "";
  const std::string step_id = args.isMember("step_id") && args["step_id"].isString() ? args["step_id"].asString() : "";
  const std::string idempotency_key =
    args.isMember("idempotency_key") && args["idempotency_key"].isString() ? args["idempotency_key"].asString() : "";
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? args["mode"].asString() : "";
  const int64_t deadline_utc_ms = args.isMember("deadline_utc_ms") && (args["deadline_utc_ms"].isInt64() || args["deadline_utc_ms"].isUInt64())
    ? (args["deadline_utc_ms"].isInt64() ? args["deadline_utc_ms"].asInt64() : (int64_t)args["deadline_utc_ms"].asUInt64())
    : 0;
  const Json::Value payload = args.isMember("payload") ? args["payload"] : Json::Value(Json::nullValue);

  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid task fields\"}";
    return;
  }

  std::unordered_set<std::string> allow_hazards;
  if (args.isMember("allow_hazards") && args["allow_hazards"].isArray()) {
    for (Json::ArrayIndex i = 0; i < args["allow_hazards"].size(); i++) {
      if (args["allow_hazards"][i].isString()) allow_hazards.insert(args["allow_hazards"][i].asString());
    }
  }
  const bool allow_high_side_effect =
    args.isMember("allow_high_side_effect") && args["allow_high_side_effect"].isBool() ? args["allow_high_side_effect"].asBool() : false;

  int64_t outbox_id = 0;
  bool deduped = false;
  std::string derr;
  int http = 500;
  if (!edge_enqueue_task_assign(
        db_or_null,
        node_id,
        task_id,
        step_id,
        idempotency_key,
        mode,
        deadline_utc_ms,
        payload,
        allow_hazards,
        allow_high_side_effect,
        /*enforce_safety=*/true,
        /*enforce_rate_limit=*/true,
        &outbox_id,
        &deduped,
        &derr,
        &http)) {
    resp->status = http;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = derr.empty() ? "failed to assign edge task" : derr;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = node_id;
  o["task_id"] = task_id;
  o["step_id"] = step_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  o["deduped"] = deduped;
  resp->body = json_stringify_compact(o);
}

void handle_edge_task_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto tid = query_get(req.query, "task_id");
  const auto sid = query_get(req.query, "step_id");
  if (!tid || tid->empty() || !sid || sid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing task_id/step_id\"}";
    return;
  }

  AgentDb::EdgeTaskRow tr;
  std::string err;
  if (!db_or_null->get_edge_task(*tid, *sid, &tr, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"task not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value t(Json::objectValue);
  t["task_id"] = tr.task_id;
  t["step_id"] = tr.step_id;
  t["node_id"] = tr.node_id;
  t["idempotency_key"] = tr.idempotency_key;
  t["mode"] = tr.mode;
  t["deadline_utc_ms"] = (Json::Int64)tr.deadline_utc_ms;
  t["state"] = tr.state;
  t["created_utc_ms"] = (Json::Int64)tr.created_utc_ms;
  t["updated_utc_ms"] = (Json::Int64)tr.updated_utc_ms;
  if (!tr.error.empty()) t["error"] = tr.error;
  if (!tr.payload_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.payload_json, &v, &perr2) && v.isObject()) t["payload"] = v;
  }
  if (!tr.result_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.result_json, &v, &perr2)) t["result"] = v;
  }
  o["task"] = t;
  resp->body = json_stringify_compact(o);
}

void handle_edge_rule_upsert_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  std::string rule_id = args.isMember("rule_id") && args["rule_id"].isString() ? trim_copy(args["rule_id"].asString()) : "";
  if (rule_id.empty()) rule_id = std::string("rule:") + make_uuidish_msg_id();
  if (!id_is_safe(rule_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid rule_id\"}";
    return;
  }

  const std::string event_type =
    args.isMember("event_type") && args["event_type"].isString() ? trim_copy(args["event_type"].asString()) : "";
  if (event_type.empty() || !id_is_safe(event_type)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid event_type\"}";
    return;
  }

  const bool enabled = args.isMember("enabled") && args["enabled"].isBool() ? args["enabled"].asBool() : true;

  double min_conf = 0.0;
  if (args.isMember("min_confidence") && (args["min_confidence"].isDouble() || args["min_confidence"].isInt())) {
    min_conf = args["min_confidence"].asDouble();
    if (min_conf < 0.0) min_conf = 0.0;
    if (min_conf > 1.0) min_conf = 1.0;
  }

  int cooldown_ms = 0;
  if (args.isMember("cooldown_ms") && (args["cooldown_ms"].isInt() || args["cooldown_ms"].isUInt())) {
    cooldown_ms = args["cooldown_ms"].isInt() ? std::max(0, args["cooldown_ms"].asInt()) : (int)args["cooldown_ms"].asUInt();
  }

  Json::Value action = args.isMember("action") ? args["action"] : Json::Value(Json::nullValue);
  if (!action.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid action (expected object)\"}";
    return;
  }
  const std::string atype = action.isMember("type") && action["type"].isString() ? action["type"].asString() : "";
  if (atype != "task_assign") {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"unsupported action.type (expected task_assign)\"}";
    return;
  }
  const std::string action_json = json_stringify_compact(action);
  if (action_json.size() > 20000) {
    resp->status = 413;
    resp->body = "{\"ok\":false,\"error\":\"action too large\"}";
    return;
  }

  const int64_t now = unix_ms_now();
  AgentDb::EdgeRuleRow existing;
  std::string gerr;
  const bool have_existing = db_or_null->get_edge_rule(rule_id, &existing, &gerr);

  AgentDb::EdgeRuleRow row;
  row.rule_id = rule_id;
  row.enabled = enabled;
  row.event_type = event_type;
  row.min_confidence = min_conf;
  row.cooldown_ms = cooldown_ms;
  row.action_json = action_json;
  row.updated_utc_ms = now;
  if (have_existing) {
    row.created_utc_ms = existing.created_utc_ms;
    row.last_fired_utc_ms = existing.last_fired_utc_ms;
  } else {
    row.created_utc_ms = now;
    row.last_fired_utc_ms = 0;
  }

  std::string err;
  if (!db_or_null->upsert_edge_rule(row, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to upsert rule";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rule_id"] = rule_id;
  resp->body = json_stringify_compact(o);
}

void handle_edge_rules_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  size_t limit = 200;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 512));

  std::vector<AgentDb::EdgeRuleRow> rules;
  std::string err;
  if (!db_or_null->list_edge_rules(limit, &rules, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list rules";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  for (const auto& r : rules) {
    Json::Value row(Json::objectValue);
    row["rule_id"] = r.rule_id;
    row["enabled"] = r.enabled;
    row["event_type"] = r.event_type;
    row["min_confidence"] = r.min_confidence;
    row["cooldown_ms"] = r.cooldown_ms;
    row["last_fired_utc_ms"] = (Json::Int64)r.last_fired_utc_ms;
    row["created_utc_ms"] = (Json::Int64)r.created_utc_ms;
    row["updated_utc_ms"] = (Json::Int64)r.updated_utc_ms;
    if (!r.action_json.empty()) {
      Json::Value a;
      std::string perr2;
      if (json_parse_any(r.action_json, &a, &perr2) && a.isObject()) row["action"] = a;
      else row["action_raw"] = r.action_json;
    }
    arr.append(row);
  }
  o["rules"] = arr;
  resp->body = json_stringify_compact(o);
}

void handle_edge_rule_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto rid = query_get(req.query, "rule_id");
  if (!rid || rid->empty() || !id_is_safe(*rid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid rule_id\"}";
    return;
  }

  std::string err;
  if (!db_or_null->delete_edge_rule(*rid, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to delete rule";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["rule_id"] = *rid;
  resp->body = json_stringify_compact(o);
}

void handle_edge_workflow_submit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  std::string workflow_id = args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) workflow_id = std::string("wf:") + make_uuidish_msg_id();
  if (!id_is_safe(workflow_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid workflow_id\"}";
    return;
  }

  std::string goal = args.isMember("goal") && args["goal"].isString() ? args["goal"].asString() : "";
  int priority = 0;
  if (args.isMember("priority") && (args["priority"].isInt() || args["priority"].isUInt())) {
    priority = args["priority"].isInt() ? args["priority"].asInt() : (int)std::min((Json::UInt)INT32_MAX, args["priority"].asUInt());
  }

  if (!args.isMember("steps") || !args["steps"].isArray() || args["steps"].empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid steps (expected non-empty array)\"}";
    return;
  }

  const int64_t now = unix_ms_now();
  std::vector<AgentDb::EdgeWorkflowStepRow> steps;
  steps.reserve(args["steps"].size());

  for (Json::ArrayIndex i = 0; i < args["steps"].size(); i++) {
    const auto& s = args["steps"][i];
    if (!s.isObject()) continue;
    const std::string step_id = s.isMember("step_id") && s["step_id"].isString() ? trim_copy(s["step_id"].asString()) : "";
    const std::string kind = s.isMember("kind") && s["kind"].isString() ? trim_copy(s["kind"].asString()) : "";
    if (step_id.empty() || !id_is_safe(step_id) || kind.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step (missing step_id/kind)\"}";
      return;
    }
    if (kind != "invoke_tool" && kind != "run_agent" && kind != "join") {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"unsupported step.kind\"}";
      return;
    }

    Json::Value depends(Json::arrayValue);
    if (s.isMember("depends_on") && s["depends_on"].isArray()) depends = s["depends_on"];
    Json::Value target = s.isMember("target") ? s["target"] : Json::Value(Json::objectValue);
    Json::Value payload = s.isMember("payload") ? s["payload"] : Json::Value(Json::objectValue);
    if (kind != "join" && !target.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step.target (expected object)\"}";
      return;
    }
    if (!payload.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step.payload (expected object)\"}";
      return;
    }

    std::string join_mode = s.isMember("join_mode") && s["join_mode"].isString() ? trim_copy(s["join_mode"].asString()) : "";
    if (!join_mode.empty() && join_mode != "all" && join_mode != "any") {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid join_mode (expected all|any)\"}";
      return;
    }

    int64_t deadline_utc_ms = 0;
    if (s.isMember("deadline_utc_ms") && (s["deadline_utc_ms"].isInt64() || s["deadline_utc_ms"].isUInt64())) {
      deadline_utc_ms = s["deadline_utc_ms"].isInt64() ? s["deadline_utc_ms"].asInt64() : (int64_t)s["deadline_utc_ms"].asUInt64();
    }
    if (kind != "join" && deadline_utc_ms <= 0) deadline_utc_ms = now + 60000;

    AgentDb::EdgeWorkflowStepRow row;
    row.workflow_id = workflow_id;
    row.step_id = step_id;
    row.kind = kind;
    row.depends_on_json = json_stringify_compact(depends);
    row.target_json = json_stringify_compact(target);
    row.payload_json = json_stringify_compact(payload);
    row.join_mode = join_mode;
    row.deadline_utc_ms = deadline_utc_ms;
    row.state = "PENDING";
    row.created_utc_ms = now;
    row.updated_utc_ms = now;
    steps.push_back(std::move(row));
  }

  if (steps.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"no valid steps\"}";
    return;
  }

  // Persist the original spec with the finalized workflow_id.
  args["workflow_id"] = workflow_id;
  const std::string spec_json = json_stringify_compact(args);

  AgentDb::EdgeWorkflowRow wf;
  wf.workflow_id = workflow_id;
  wf.goal = goal;
  wf.status = "QUEUED";
  wf.priority = priority;
  wf.spec_json = spec_json;
  wf.created_utc_ms = now;
  wf.updated_utc_ms = now;

  std::string err;
  if (!db_or_null->create_edge_workflow(wf, steps, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to create edge workflow";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  resp->body = json_stringify_compact(o);
}

void handle_edge_workflow_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty() || !id_is_safe(*wid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid workflow_id\"}";
    return;
  }
  bool include_steps = false;
  const auto inc = query_get(req.query, "include_steps");
  if (inc && !inc->empty()) {
    const std::string v = lower_copy(*inc);
    include_steps = (v == "1" || v == "true" || v == "yes");
  }

  AgentDb::EdgeWorkflowRow wf;
  std::string err;
  if (!db_or_null->get_edge_workflow(*wid, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value w(Json::objectValue);
  w["workflow_id"] = wf.workflow_id;
  if (!wf.goal.empty()) w["goal"] = wf.goal;
  w["status"] = wf.status;
  w["priority"] = wf.priority;
  w["created_utc_ms"] = (Json::Int64)wf.created_utc_ms;
  w["updated_utc_ms"] = (Json::Int64)wf.updated_utc_ms;
  if (!wf.error.empty()) w["error"] = wf.error;
  if (!wf.spec_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(wf.spec_json, &v, &perr2) && v.isObject()) w["spec"] = v;
  }
  o["workflow"] = w;

  if (include_steps) {
    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    std::string serr;
    if (db_or_null->list_edge_workflow_steps(*wid, &steps, &serr)) {
      Json::Value arr(Json::arrayValue);
      std::string perr2;
      for (const auto& s : steps) {
        Json::Value r(Json::objectValue);
        r["step_id"] = s.step_id;
        r["kind"] = s.kind;
        r["state"] = s.state;
        if (!s.join_mode.empty()) r["join_mode"] = s.join_mode;
        if (s.deadline_utc_ms > 0) r["deadline_utc_ms"] = (Json::Int64)s.deadline_utc_ms;
        r["created_utc_ms"] = (Json::Int64)s.created_utc_ms;
        r["updated_utc_ms"] = (Json::Int64)s.updated_utc_ms;
        if (!s.error.empty()) r["error"] = s.error;
        Json::Value v;
        if (json_parse_any(s.depends_on_json, &v, &perr2) && v.isArray()) r["depends_on"] = v;
        if (json_parse_any(s.target_json, &v, &perr2) && v.isObject()) r["target"] = v;
        if (json_parse_any(s.payload_json, &v, &perr2) && v.isObject()) r["payload"] = v;
        arr.append(r);
      }
      o["steps"] = arr;
    }
  }
  resp->body = json_stringify_compact(o);
}

void handle_edge_workflow_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto st = query_get(req.query, "status");
  std::string status = st && !st->empty() ? trim_copy(*st) : "QUEUED";
  if (status != "QUEUED" && status != "RUNNING" && status != "SUCCEEDED" && status != "FAILED" && status != "CANCELED") {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid status\"}";
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 256));

  std::vector<AgentDb::EdgeWorkflowRow> rows;
  std::string err;
  if (!db_or_null->list_edge_workflows_by_status(status, limit, &rows, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflows";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["status"] = status;
  Json::Value arr(Json::arrayValue);
  for (const auto& wf : rows) {
    Json::Value w(Json::objectValue);
    w["workflow_id"] = wf.workflow_id;
    if (!wf.goal.empty()) w["goal"] = wf.goal;
    w["status"] = wf.status;
    w["priority"] = wf.priority;
    w["created_utc_ms"] = (Json::Int64)wf.created_utc_ms;
    w["updated_utc_ms"] = (Json::Int64)wf.updated_utc_ms;
    if (!wf.error.empty()) w["error"] = wf.error;
    arr.append(w);
  }
  o["workflows"] = arr;
  resp->body = json_stringify_compact(o);
}

}  // namespace agentd
