#include "session_voice_runtime_store.h"

#include "json_util.h"
#include "session_voice_backend_policy.h"
#include "session_voice_broker_client.h"
#include "session_voice_child_runtime.h"
#include "string_util.h"

#include <chrono>
#include <set>
#include <utility>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string voice_peer_meta_key(const std::string& session_id) {
  return "session.voice_webrtc_peer." + session_id;
}

}  // namespace

Json::Value voice_peer_runtime_to_json(const VoicePeerRuntime& st) {
  Json::Value out(Json::objectValue);
  out["schema"] = "session_voice_webrtc_peer_runtime_v1";
  out["runtime_kind"] = st.runtime_kind;
  out["status_source"] = st.status_source.empty() ? "memory" : st.status_source;
  out["session_id"] = st.session_id;
  if (!st.broker_session_id.empty()) out["broker_session_id"] = st.broker_session_id;
  out["broker_url"] = st.broker_url;
  out["managed_broker_session"] = st.managed_broker_session;
  if (!st.broker_agent_id.empty()) out["broker_agent_id"] = st.broker_agent_id;
  if (!st.broker_deployment_id.empty()) out["broker_deployment_id"] = st.broker_deployment_id;
  out["sender_tag"] = st.sender_tag;
  out["tool_path"] = st.tool_path;
  out["node_bin"] = st.node_bin;
  if (!st.stdout_log_path.empty()) out["stdout_log_path"] = st.stdout_log_path;
  out["started_unix_ms"] = (Json::Int64)st.started_unix_ms;
  if (st.ended_unix_ms > 0) out["ended_unix_ms"] = (Json::Int64)st.ended_unix_ms;
  out["deadline_ms"] = (Json::Int64)st.deadline_ms;
  out["poll_interval_ms"] = (Json::Int64)st.poll_interval_ms;
  out["tone_hz"] = (Json::Int64)st.tone_hz;
  out["ready"] = st.ready;
  out["running"] = st.running;
  if (!st.stderr_log_path.empty()) out["stderr_log_path"] = st.stderr_log_path;
  if (!st.ready_file_path.empty()) out["ready_file_path"] = st.ready_file_path;
  if (st.running) {
    out["pid"] = (Json::Int64)st.pid;
  } else {
    out["exit_code"] = st.exit_code;
    if (st.exit_signal != 0) out["exit_signal"] = st.exit_signal;
  }
  if (!st.last_error.empty()) out["last_error"] = st.last_error;
  if (!st.last_stdout_line.empty()) out["last_stdout_line"] = st.last_stdout_line;
  if (!st.last_stdout_json.isNull()) out["last_stdout"] = st.last_stdout_json;
  return out;
}

Json::Value voice_peer_runtime_backend_policy_drift_json(const DaemonConfig& cfg, const VoicePeerRuntime& st) {
  if (!st.running) return Json::Value(Json::nullValue);

  Json::Value changed_fields(Json::arrayValue);
  std::set<std::string> seen_changed_fields;
  auto add_changed_field = [&](const std::string& name) {
    if (name.empty()) return;
    if (!seen_changed_fields.insert(name).second) return;
    changed_fields.append(name);
  };

  const std::string current_default_runtime_kind = default_voice_peer_runtime_kind(cfg);
  const std::string current_default_runtime_kind_source = default_voice_peer_runtime_kind_source(cfg);
  const std::string current_broker_url = effective_voice_broker_url(cfg, "");
  const std::string default_unavailable_reason =
    voice_peer_backend_unavailable_reason(cfg, current_default_runtime_kind);

  Json::Value current_effective_start(Json::objectValue);
  current_effective_start["runtime_kind"] = current_default_runtime_kind;
  current_effective_start["default_runtime_kind_source"] = current_default_runtime_kind_source;
  current_effective_start["broker_url_configured"] = !current_broker_url.empty();
  if (!current_broker_url.empty()) current_effective_start["broker_url"] = current_broker_url;
  current_effective_start["runtime_available"] = default_unavailable_reason.empty();
  if (!default_unavailable_reason.empty()) {
    current_effective_start["runtime_unavailable_reason"] = default_unavailable_reason;
  }

  if (current_default_runtime_kind != st.runtime_kind) add_changed_field("default_runtime_kind");
  if (current_broker_url != st.broker_url) add_changed_field("broker_url_default");

  if (current_default_runtime_kind == st.runtime_kind &&
      (current_default_runtime_kind == "bundled" || current_default_runtime_kind == "external")) {
    std::string resolved_tool_path;
    std::string resolved_node_bin;
    std::string resolved_err;
    (void)resolve_voice_peer_backend(
      cfg, current_default_runtime_kind, &resolved_tool_path, &resolved_node_bin, &resolved_err);
    const std::string effective_node_bin = trim_copy(
      cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);
    current_effective_start["node_bin"] = effective_node_bin;

    std::string effective_tool_path = resolved_tool_path;
    if (effective_tool_path.empty()) {
      if (current_default_runtime_kind == "bundled") {
        effective_tool_path = discover_bundled_audio_peer_tool_path(cfg);
      } else {
        effective_tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
      }
    }
    if (!effective_tool_path.empty()) current_effective_start["tool_path"] = effective_tool_path;

    if (effective_node_bin != st.node_bin) add_changed_field("node_bin");
    if (effective_tool_path != st.tool_path) {
      add_changed_field(current_default_runtime_kind == "bundled" ? "bundled_tool_path" : "peer_tool_path");
    }
  }

  if (changed_fields.empty()) return Json::Value(Json::nullValue);

  Json::Value out(Json::objectValue);
  out["changed_fields"] = changed_fields;
  out["current_effective_start"] = current_effective_start;
  return out;
}

void voice_peer_add_runtime_snapshot(const DaemonConfig& cfg, const VoicePeerRuntime& st, Json::Value* out) {
  if (!out) return;
  (*out)["peer"] = voice_peer_runtime_to_json(st);
  const Json::Value drift = voice_peer_runtime_backend_policy_drift_json(cfg, st);
  if (!drift.isNull()) (*out)["backend_policy_drift"] = drift;
}

bool voice_peer_runtime_matches_start_request(
  const VoicePeerRuntime& st,
  const Json::Value& body,
  const std::string& runtime_kind,
  const std::string& effective_broker_url,
  const std::string& desired_tool_path,
  const std::string& desired_node_bin,
  const std::string& requested_broker_session_id,
  const std::string& broker_agent_id,
  const std::string& broker_deployment_id,
  const std::string& sender_tag,
  int64_t deadline_ms,
  int64_t poll_interval_ms,
  int64_t tone_hz
) {
  if (runtime_kind != st.runtime_kind) return false;
  if (runtime_kind == "bundled" || runtime_kind == "external") {
    if (desired_tool_path != st.tool_path) return false;
    if (desired_node_bin != st.node_bin) return false;
  }
  if (!effective_broker_url.empty() && effective_broker_url != st.broker_url) return false;
  if (body.isMember("broker_session_id") && body["broker_session_id"].isString() &&
      (!requested_broker_session_id.empty() &&
       (requested_broker_session_id != st.broker_session_id || st.managed_broker_session))) {
    return false;
  }
  if ((body.isMember("broker_agent_id") && body["broker_agent_id"].isString()) ||
      (body.isMember("broker_deployment_id") && body["broker_deployment_id"].isString())) {
    if (!st.managed_broker_session) return false;
    if (broker_agent_id != st.broker_agent_id) return false;
    if (broker_deployment_id != st.broker_deployment_id) return false;
  }
  if (body.isMember("sender_tag") && body["sender_tag"].isString() && sender_tag != st.sender_tag) return false;
  if (body.isMember("deadline_ms") &&
      (body["deadline_ms"].isInt64() || body["deadline_ms"].isUInt64() || body["deadline_ms"].isInt()) &&
      deadline_ms != st.deadline_ms) {
    return false;
  }
  if (body.isMember("poll_interval_ms") &&
      (body["poll_interval_ms"].isInt64() || body["poll_interval_ms"].isUInt64() || body["poll_interval_ms"].isInt()) &&
      poll_interval_ms != st.poll_interval_ms) {
    return false;
  }
  if (body.isMember("tone_hz") &&
      (body["tone_hz"].isInt64() || body["tone_hz"].isUInt64() || body["tone_hz"].isInt()) &&
      tone_hz != st.tone_hz) {
    return false;
  }
  return true;
}

bool voice_peer_runtime_from_json(const Json::Value& v, VoicePeerRuntime* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  if (!v.isObject()) {
    if (out_err) *out_err = "runtime record must be an object";
    return false;
  }
  VoicePeerRuntime st;
  if (v.isMember("runtime_kind") && v["runtime_kind"].isString()) st.runtime_kind = trim_copy(v["runtime_kind"].asString());
  if (v.isMember("status_source") && v["status_source"].isString()) st.status_source = trim_copy(v["status_source"].asString());
  if (v.isMember("session_id") && v["session_id"].isString()) st.session_id = trim_copy(v["session_id"].asString());
  if (v.isMember("broker_session_id") && v["broker_session_id"].isString()) st.broker_session_id = trim_copy(v["broker_session_id"].asString());
  if (v.isMember("broker_url") && v["broker_url"].isString()) st.broker_url = v["broker_url"].asString();
  if (v.isMember("broker_agent_id") && v["broker_agent_id"].isString()) st.broker_agent_id = trim_copy(v["broker_agent_id"].asString());
  if (v.isMember("broker_deployment_id") && v["broker_deployment_id"].isString()) st.broker_deployment_id = trim_copy(v["broker_deployment_id"].asString());
  if (v.isMember("sender_tag") && v["sender_tag"].isString()) st.sender_tag = trim_copy(v["sender_tag"].asString());
  if (v.isMember("tool_path") && v["tool_path"].isString()) st.tool_path = v["tool_path"].asString();
  if (v.isMember("node_bin") && v["node_bin"].isString()) st.node_bin = trim_copy(v["node_bin"].asString());
  if (v.isMember("ready_file_path") && v["ready_file_path"].isString()) st.ready_file_path = v["ready_file_path"].asString();
  if (v.isMember("stdout_log_path") && v["stdout_log_path"].isString()) st.stdout_log_path = v["stdout_log_path"].asString();
  if (v.isMember("stderr_log_path") && v["stderr_log_path"].isString()) st.stderr_log_path = v["stderr_log_path"].asString();
  if (v.isMember("started_unix_ms") && (v["started_unix_ms"].isInt64() || v["started_unix_ms"].isUInt64())) st.started_unix_ms = v["started_unix_ms"].asInt64();
  if (v.isMember("ended_unix_ms") && (v["ended_unix_ms"].isInt64() || v["ended_unix_ms"].isUInt64())) st.ended_unix_ms = v["ended_unix_ms"].asInt64();
  if (v.isMember("deadline_ms") && (v["deadline_ms"].isInt64() || v["deadline_ms"].isUInt64())) st.deadline_ms = v["deadline_ms"].asInt64();
  if (v.isMember("poll_interval_ms") && (v["poll_interval_ms"].isInt64() || v["poll_interval_ms"].isUInt64())) st.poll_interval_ms = v["poll_interval_ms"].asInt64();
  if (v.isMember("tone_hz") && (v["tone_hz"].isInt64() || v["tone_hz"].isUInt64())) st.tone_hz = v["tone_hz"].asInt64();
  if (v.isMember("managed_broker_session") && v["managed_broker_session"].isBool()) {
    st.managed_broker_session = v["managed_broker_session"].asBool();
  }
  if (v.isMember("ready") && v["ready"].isBool()) st.ready = v["ready"].asBool();
  if (v.isMember("running") && v["running"].isBool()) st.running = v["running"].asBool();
  if (v.isMember("exit_code") && v["exit_code"].isInt()) st.exit_code = v["exit_code"].asInt();
  if (v.isMember("exit_signal") && v["exit_signal"].isInt()) st.exit_signal = v["exit_signal"].asInt();
  if (v.isMember("last_error") && v["last_error"].isString()) st.last_error = v["last_error"].asString();
  if (v.isMember("last_stdout_line") && v["last_stdout_line"].isString()) st.last_stdout_line = v["last_stdout_line"].asString();
  if (v.isMember("last_stdout") && v["last_stdout"].isObject()) st.last_stdout_json = v["last_stdout"];
  if (v.isMember("pid") && (v["pid"].isInt64() || v["pid"].isUInt64())) st.pid = (decltype(st.pid))v["pid"].asInt64();
  *out = std::move(st);
  return true;
}

bool persist_voice_peer_runtime_record(AgentDb* db, const VoicePeerRuntime& st, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  Json::Value record = voice_peer_runtime_to_json(st);
  record["persisted_utc_ms"] = (Json::Int64)now_unix_ms();
  return db->meta_set(voice_peer_meta_key(st.session_id), json_stringify(record), out_err);
}

bool clear_voice_peer_runtime_record(AgentDb* db, const std::string& session_id, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  return db->meta_set(voice_peer_meta_key(session_id), "", out_err);
}

bool load_voice_peer_runtime_record(
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  bool* out_self_healed,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_self_healed) *out_self_healed = false;
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  std::string raw;
  if (!db->meta_get(voice_peer_meta_key(session_id), &raw, out_err)) return false;
  if (trim_copy(raw).empty()) return true;
  Json::Value parsed(Json::nullValue);
  std::string jerr;
  if (!json_parse_any(raw, &parsed, &jerr) || !parsed.isObject()) {
    std::string cerr;
    if (!clear_voice_peer_runtime_record(db, session_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? (jerr.empty() ? "persisted voice runtime record corrupt" : jerr)
          : ("failed to clear corrupt persisted voice runtime record: " + cerr);
      }
      return false;
    }
    if (out_self_healed) *out_self_healed = true;
    return true;
  }
  auto st = std::make_shared<VoicePeerRuntime>();
  if (!voice_peer_runtime_from_json(parsed, st.get(), out_err)) {
    const std::string original_err = out_err ? *out_err : std::string("persisted voice runtime record corrupt");
    std::string cerr;
    if (!clear_voice_peer_runtime_record(db, session_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? original_err
          : ("failed to clear corrupt persisted voice runtime record: " + cerr);
      }
      return false;
    }
    if (out_state) out_state->reset();
    if (out_self_healed) *out_self_healed = true;
    if (out_err) out_err->clear();
    return true;
  }
  const bool persisted_claimed_running = st->running;
  st->status_source = "persisted";
  refresh_voice_peer_runtime_state(st.get());
  st->stale_persisted_record = persisted_claimed_running && !st->running;
  if (out_state) *out_state = std::move(st);
  return true;
}

Json::Value cleanup_stale_persisted_voice_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id
) {
  Json::Value cleanup(Json::objectValue);
  cleanup["persisted_record_cleared"] = clear_voice_peer_runtime_record(db, session_id, nullptr);
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  return cleanup;
}

Json::Value voice_peer_corrupt_record_cleanup_json(const DaemonConfig& cfg, const std::string& session_id) {
  Json::Value cleanup(Json::objectValue);
  cleanup["persisted_record_cleared"] = true;
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_voice_peer_runtime_artifacts(cfg, session_id, &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  return cleanup;
}

}  // namespace agentd
