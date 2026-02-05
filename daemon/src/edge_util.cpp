#include "edge_util.h"

#include "json_util.h"
#include "string_util.h"
#include "trace_id_util.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <random>
#include <sstream>

namespace agentd {
namespace {

static std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool json_value_type_matches_string(const Json::Value& v, const std::string& type) {
  if (type == "object") return v.isObject();
  if (type == "array") return v.isArray();
  if (type == "string") return v.isString();
  if (type == "boolean") return v.isBool();
  if (type == "integer") return v.isInt() || v.isInt64() || v.isUInt() || v.isUInt64();
  if (type == "number") return v.isNumeric();
  if (type == "null") return v.isNull();
  return true; // unknown type => best-effort allow
}

static bool json_schema_subset_validate_best_effort_impl(
  const Json::Value& schema,
  const Json::Value& value,
  const std::string& path,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!schema.isObject()) return true;

  const bool has_object_keywords =
    schema.isMember("properties") || schema.isMember("required") || schema.isMember("additionalProperties");
  const bool has_array_keywords = schema.isMember("items");

  // enum
  if (schema.isMember("enum") && schema["enum"].isArray() && !schema["enum"].empty()) {
    bool ok = false;
    for (Json::ArrayIndex i = 0; i < schema["enum"].size(); i++) {
      if (schema["enum"][i] == value) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      if (out_error) *out_error = "schema enum mismatch at " + path;
      return false;
    }
  }

  // type
  std::string type;
  if (schema.isMember("type") && schema["type"].isString()) {
    type = schema["type"].asString();
    if (!type.empty() && !json_value_type_matches_string(value, type)) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected " + type + ")";
      return false;
    }
  }

  // object specifics
  const bool is_object_schema = (type == "object") || (!type.empty() ? false : has_object_keywords);
  if (is_object_schema) {
    if (!value.isObject()) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected object)";
      return false;
    }

    if (schema.isMember("required") && schema["required"].isArray()) {
      for (Json::ArrayIndex i = 0; i < schema["required"].size(); i++) {
        if (!schema["required"][i].isString()) continue;
        const std::string k = schema["required"][i].asString();
        if (k.empty()) continue;
        if (!value.isMember(k)) {
          if (out_error) *out_error = "missing required property at " + path + ": " + k;
          return false;
        }
      }
    }

    const bool additional_props_allowed =
      !schema.isMember("additionalProperties") ||
      (!schema["additionalProperties"].isBool() || schema["additionalProperties"].asBool());

    const Json::Value props =
      schema.isMember("properties") && schema["properties"].isObject() ? schema["properties"] : Json::Value(Json::nullValue);

    if (!additional_props_allowed) {
      for (const auto& k : value.getMemberNames()) {
        if (!props.isObject() || !props.isMember(k)) {
          if (out_error) *out_error = "unknown property at " + path + ": " + k;
          return false;
        }
      }
    }

    if (props.isObject()) {
      for (const auto& k : value.getMemberNames()) {
        if (!props.isMember(k) || !props[k].isObject()) continue;
        std::string err;
        const std::string child_path = path.empty() ? k : (path + "." + k);
        if (!json_schema_subset_validate_best_effort_impl(props[k], value[k], child_path, &err)) {
          if (out_error) *out_error = err;
          return false;
        }
      }
    }
  }

  // array basics (subset): items schema applied when present.
  const bool is_array_schema = (type == "array") || (!type.empty() ? false : has_array_keywords);
  if (is_array_schema) {
    if (!value.isArray()) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected array)";
      return false;
    }
    if (schema.isMember("items") && schema["items"].isObject()) {
      const Json::Value items = schema["items"];
      for (Json::ArrayIndex i = 0; i < value.size(); i++) {
        std::string err;
        const std::string child_path = path + "[" + std::to_string((int)i) + "]";
        if (!json_schema_subset_validate_best_effort_impl(items, value[i], child_path, &err)) {
          if (out_error) *out_error = err;
          return false;
        }
      }
    }
  }

  return true;
}

static bool edge_tool_parameters_schema_from_manifest_best_effort_impl(
  const Json::Value& manifest,
  const std::string& tool_name,
  Json::Value* out_schema,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_schema) *out_schema = Json::Value(Json::nullValue);
  if (!out_schema) return false;
  if (!manifest.isObject() || tool_name.empty()) return true;
  if (!manifest.isMember("tools") || !manifest["tools"].isArray()) return true;
  const Json::Value tools = manifest["tools"];
  for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
    if (!tools[i].isObject()) continue;
    const Json::Value t = tools[i];
    const std::string name = t.isMember("name") && t["name"].isString() ? trim_copy(t["name"].asString()) : "";
    if (name != tool_name) continue;
    if (t.isMember("parameters_schema") && t["parameters_schema"].isObject()) {
      *out_schema = t["parameters_schema"];
      return true;
    }
    // Fallback for OpenAI-style naming.
    if (t.isMember("parameters") && t["parameters"].isObject()) {
      *out_schema = t["parameters"];
      return true;
    }
    return true; // tool found but schema missing => allow
  }
  if (out_error) *out_error = "tool not found in manifest";
  return false;
}

}  // namespace

bool edge_trace_id_is_safe(const std::string& s) {
  return trace_id_is_safe(s);
}

bool edge_json_schema_subset_validate_best_effort(
  const Json::Value& schema,
  const Json::Value& value,
  const std::string& path,
  std::string* out_error
) {
  return json_schema_subset_validate_best_effort_impl(schema, value, path, out_error);
}

bool edge_tool_parameters_schema_from_manifest_best_effort(
  const Json::Value& manifest,
  const std::string& tool_name,
  Json::Value* out_schema,
  std::string* out_error
) {
  return edge_tool_parameters_schema_from_manifest_best_effort_impl(manifest, tool_name, out_schema, out_error);
}

bool edge_tool_result_schema_from_manifest_best_effort(
  const Json::Value& manifest,
  const std::string& tool_name,
  Json::Value* out_schema,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_schema) *out_schema = Json::Value(Json::nullValue);
  if (!out_schema) return false;
  if (!manifest.isObject() || tool_name.empty()) return true;
  if (!manifest.isMember("tools") || !manifest["tools"].isArray()) return true;
  const Json::Value tools = manifest["tools"];
  for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
    if (!tools[i].isObject()) continue;
    const Json::Value t = tools[i];
    const std::string name = t.isMember("name") && t["name"].isString() ? trim_copy(t["name"].asString()) : "";
    if (name != tool_name) continue;
    if (t.isMember("result_schema") && t["result_schema"].isObject()) {
      *out_schema = t["result_schema"];
      return true;
    }
    return true; // tool found but schema missing => allow
  }
  if (out_error) *out_error = "tool not found in manifest";
  return false;
}

int64_t edge_unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string edge_json_stringify_compact(const Json::Value& v) {
  return json_stringify_compact_local(v);
}

bool edge_id_is_safe(const std::string& s) {
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

bool edge_sha256_token_is_safe(const std::string& s) {
  if (s.empty()) return false;
  const std::string prefix = "sha256:";
  size_t off = 0;
  if (s.size() > prefix.size() && s.rfind(prefix, 0) == 0) off = prefix.size();
  const size_t n = s.size() - off;
  if (n != 64) return false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[off + i];
    const bool ok =
      (c >= '0' && c <= '9') ||
      (c >= 'a' && c <= 'f') ||
      (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

std::string edge_make_uuidish_msg_id() {
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

std::string edge_node_to_prefix(const std::string& node_id) {
  if (node_id.empty()) return "";
  if (node_id.rfind("node:", 0) == 0) return node_id;
  return std::string("node:") + node_id;
}

bool edge_parse_string_set(const std::string& json, std::unordered_set<std::string>* out) {
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

void edge_manifest_extract_best_effort(
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
    if (tags.isArray()) *out_tags_json = json_stringify_compact_local(tags);
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
    *out_tools_json = json_stringify_compact_local(arr);
  }

  if (out_hw_presence_json) {
    const auto& hw = manifest["hardware"];
    if (hw.isObject()) {
      const auto& pres = hw["presence"];
      if (pres.isObject()) *out_hw_presence_json = json_stringify_compact_local(pres);
    }
  }
}

bool edge_select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  const std::unordered_set<std::string>* exclude_node_ids_or_null,
  std::string* out_node_id
) {
  if (out_node_id) out_node_id->clear();
  if (!db || !db->is_open() || !out_node_id) return false;

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db->list_edge_nodes(/*max_rows=*/256, &nodes, &err)) return false;

  for (const auto& n : nodes) {
    if (n.node_id.empty()) continue;
    if (exclude_node_ids_or_null && exclude_node_ids_or_null->count(n.node_id)) continue;

    std::unordered_set<std::string> toolset;
    std::unordered_set<std::string> tagset;
    if (!edge_parse_string_set(n.tools_json, &toolset)) continue;
    if (!edge_parse_string_set(n.tags_json, &tagset)) continue;

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

bool edge_is_terminal_task_state(const std::string& s) {
  return s == "SUCCEEDED" || s == "FAILED" || s == "TIMED_OUT" || s == "CANCELED";
}

bool edge_tool_meta_from_manifest(
  const Json::Value& manifest,
  const std::string& tool_name,
  EdgeToolMeta* out_meta,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_meta) *out_meta = EdgeToolMeta{};
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

    EdgeToolMeta m;
    if (t.isMember("side_effect_level") && t["side_effect_level"].isString()) {
      m.side_effect_level = t["side_effect_level"].asString();
    }
    if (t.isMember("resource_lock") && t["resource_lock"].isString()) {
      m.resource_lock = trim_copy(t["resource_lock"].asString());
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

bool edge_rate_limit_check_best_effort(
  AgentDb* db,
  const std::string& node_id,
  const std::string& tool_name,
  const EdgeToolMeta& meta,
  int64_t now_utc_ms,
  AgentDb::EdgeToolRateStateRow* out_next_state,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_next_state) *out_next_state = AgentDb::EdgeToolRateStateRow{};
  if (!db || !db->is_open()) return false;
  if (!meta.has_rate_limit) return true;
  if (node_id.empty() || tool_name.empty()) return true;
  const int64_t now = now_utc_ms > 0 ? now_utc_ms : edge_unix_ms_now();

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

void edge_rate_limit_commit_best_effort(AgentDb* db, const AgentDb::EdgeToolRateStateRow& next_state) {
  if (!db || !db->is_open()) return;
  if (next_state.node_id.empty() || next_state.tool_name.empty()) return;
  (void)db->upsert_edge_tool_rate_state(next_state, nullptr);
}

bool edge_enqueue_task_assign(
  AgentDb* db,
  const std::string& node_id,
  const std::string& task_id,
  const std::string& step_id,
  const std::string& idempotency_key,
  const std::string& mode,
  int64_t deadline_utc_ms,
  int attempt,
  const Json::Value& payload,
  const Json::Value& trace,
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
  if (node_id.empty() || !edge_id_is_safe(node_id)) {
    if (out_error) *out_error = "missing/invalid node_id";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (task_id.empty() || !edge_id_is_safe(task_id)) {
    if (out_error) *out_error = "missing/invalid task_id";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (step_id.empty() || !edge_id_is_safe(step_id)) {
    if (out_error) *out_error = "missing/invalid step_id";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (idempotency_key.empty() || !edge_id_is_safe(idempotency_key)) {
    if (out_error) *out_error = "missing/invalid idempotency_key";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    if (out_error) *out_error = "missing/invalid task fields";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  if (!trace.isNull() && !trace.isObject()) {
    if (out_error) *out_error = "invalid trace (expected object)";
    if (out_http_status) *out_http_status = 400;
    return false;
  }
  std::string trace_id;
  if (trace.isObject() && trace.isMember("trace_id") && trace["trace_id"].isString()) {
    trace_id = trim_copy(trace["trace_id"].asString());
    if (!trace_id.empty() && !edge_trace_id_is_safe(trace_id)) {
      if (out_error) *out_error = "invalid trace.trace_id";
      if (out_http_status) *out_http_status = 400;
      return false;
    }
  }
  if (attempt < 0) attempt = 0;

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
  std::string resource_lock;

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
    // Best-effort: backfill trace_id if a newer caller provides it.
    if (!trace_id.empty() && existing.trace_id.empty()) {
      existing.trace_id = trace_id;
      existing.updated_utc_ms = edge_unix_ms_now();
      (void)db->upsert_edge_task(existing, nullptr);
    }
  } else {
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

      EdgeToolMeta meta;
      std::string terr;
      if (!edge_tool_meta_from_manifest(manifest, tool_name, &meta, &terr)) {
        if (out_error) *out_error = std::string("tool not present in node manifest: ") + terr;
        if (out_http_status) *out_http_status = 409;
        return false;
      }
      if (!meta.resource_lock.empty()) resource_lock = meta.resource_lock;

      if (!resource_lock.empty()) {
        std::string ctid, csid, cerr;
        const bool conflict = db->get_edge_task_lock_conflict(node_id, resource_lock, &ctid, &csid, &cerr);
        if (conflict) {
          if (out_error) {
            *out_error = "resource_locked: " + resource_lock;
            if (!ctid.empty() && !csid.empty()) *out_error += " (held by " + ctid + "/" + csid + ")";
          }
          if (out_http_status) *out_http_status = 429;
          return false;
        }
      }

      // Validate tool arguments against the manifest schema (best-effort).
      // This is a high-leverage correctness/safety guardrail for MCU/actuator tools.
      const Json::Value args = payload.isMember("args") ? payload["args"] : Json::Value(Json::nullValue);
      if (!args.isObject()) {
        if (out_error) *out_error = "payload.args must be an object for mode=invoke";
        if (out_http_status) *out_http_status = 400;
        return false;
      }
      Json::Value schema;
      std::string serr;
      if (edge_tool_parameters_schema_from_manifest_best_effort(manifest, tool_name, &schema, &serr)) {
        if (schema.isObject()) {
          std::string verr;
          if (!edge_json_schema_subset_validate_best_effort(schema, args, "args", &verr)) {
            if (out_error) *out_error = verr.empty() ? "invalid tool args" : verr;
            if (out_http_status) *out_http_status = 400;
            return false;
          }
        }
      } else {
        if (out_error) *out_error = std::string("tool not present in node manifest: ") + serr;
        if (out_http_status) *out_http_status = 409;
        return false;
      }

      if (enforce_safety) {
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
        if (!edge_rate_limit_check_best_effort(db, node_id, tool_name, meta, edge_unix_ms_now(), &next_rate_state, &rlerr)) {
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
    if (!trace_id.empty()) tr.trace_id = trace_id;
    tr.mode = mode;
    tr.tool_name = tool_name;
    if (!resource_lock.empty()) tr.resource_lock = resource_lock;
    tr.deadline_utc_ms = deadline_utc_ms;
    tr.payload_json = json_stringify_compact_local(payload);
    tr.state = "QUEUED";
    tr.created_utc_ms = edge_unix_ms_now();
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
    if (attempt > 0) d["attempt"] = attempt;
    if (!tool_name.empty()) d["tool"] = tool_name;
    if (trace.isObject()) d["trace"] = trace;
    ev.data_json = json_stringify_compact_local(d);
    (void)db->insert_edge_task_event(ev, nullptr, nullptr);
  }

  // Enqueue UM‑BMP TASK_ASSIGN envelope to node outbox.
  const int64_t now = edge_unix_ms_now();
  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)now;
  env["type"] = "TASK_ASSIGN";
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(node_id);
  if (trace.isObject()) env["trace"] = trace;
  Json::Value b(Json::objectValue);
  b["task_id"] = task_id;
  b["step_id"] = step_id;
  b["idempotency_key"] = idempotency_key;
  b["mode"] = mode;
  b["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
  if (attempt > 0) b["attempt"] = attempt;
  b["payload"] = payload;
  env["body"] = b;

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = node_id;
  orow.ts_utc_ms = now;
  orow.envelope_json = json_stringify_compact_local(env);
  int64_t outbox_id = 0;
  if (!db->insert_edge_outbox_message(orow, &outbox_id, &err)) {
    if (out_error) *out_error = std::string("failed to enqueue outbox message: ") + err;
    if (out_http_status) *out_http_status = 500;
    return false;
  }

  if (out_outbox_id) *out_outbox_id = outbox_id;

  if (!has_existing && enforce_rate_limit && have_rate_state_to_commit) {
    edge_rate_limit_commit_best_effort(db, next_rate_state);
  }

  if (out_http_status) *out_http_status = 200;
  return true;
}

}  // namespace agentd
