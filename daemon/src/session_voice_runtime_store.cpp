#include "session_voice_runtime_store.h"

#include "json_util.h"
#include "session_voice_backend_policy.h"
#include "session_voice_backend_state.h"
#include "session_voice_broker_client.h"
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
  out["media_engine_kind"] = st.media_engine_kind;
  out["media_engine_state"] = st.media_engine_state;
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
  out["media_state_updated_unix_ms"] = (Json::Int64)st.media_state_updated_unix_ms;
  out["media_events_total"] = (Json::Int64)st.media_events_total;
  out["media_remote_offers_seen"] = (Json::Int64)st.media_remote_offers_seen;
  out["media_answers_sent"] = (Json::Int64)st.media_answers_sent;
  out["media_remote_candidates_seen"] = (Json::Int64)st.media_remote_candidates_seen;
  out["media_remote_byes_seen"] = (Json::Int64)st.media_remote_byes_seen;
  out["media_local_byes_sent"] = (Json::Int64)st.media_local_byes_sent;
  out["native_media_supported"] = st.native_media_supported;
  out["native_media_active"] = st.native_media_active;
  out["dtls_identity_ready"] = st.dtls_identity_ready;
  out["dtls_handshake_ready"] = st.dtls_handshake_ready;
  out["dtls_exporter_ready"] = st.dtls_exporter_ready;
  out["srtp_contexts_ready"] = st.srtp_contexts_ready;
  out["srtp_inbound_ready"] = st.srtp_inbound_ready;
  out["srtp_outbound_ready"] = st.srtp_outbound_ready;
  if (!st.dtls_fingerprint_sha256.empty()) out["dtls_fingerprint_sha256"] = st.dtls_fingerprint_sha256;
  if (!st.dtls_setup_role.empty()) out["dtls_setup_role"] = st.dtls_setup_role;
  if (!st.dtls_certificate_subject.empty()) out["dtls_certificate_subject"] = st.dtls_certificate_subject;
  if (!st.dtls_handshake_state.empty()) out["dtls_handshake_state"] = st.dtls_handshake_state;
  if (!st.dtls_selected_srtp_profile.empty()) {
    out["dtls_selected_srtp_profile"] = st.dtls_selected_srtp_profile;
  }
  if (!st.srtp_last_error.empty()) out["srtp_last_error"] = st.srtp_last_error;
  out["dtls_packets_sent"] = Json::Int64(st.dtls_packets_sent);
  out["dtls_packets_received"] = Json::Int64(st.dtls_packets_received);
  out["rtp_packets_received"] = Json::Int64(st.rtp_packets_received);
  out["rtp_payload_bytes_received"] = Json::Int64(st.rtp_payload_bytes_received);
  if (st.rtp_last_payload_type >= 0) out["rtp_last_payload_type"] = Json::Int64(st.rtp_last_payload_type);
  if (st.rtp_last_sequence >= 0) out["rtp_last_sequence"] = Json::Int64(st.rtp_last_sequence);
  if (st.rtp_last_timestamp > 0) out["rtp_last_timestamp"] = Json::Int64(st.rtp_last_timestamp);
  if (st.rtp_last_ssrc > 0) out["rtp_last_ssrc"] = Json::Int64(st.rtp_last_ssrc);
  out["audio_frames_decoded"] = Json::Int64(st.audio_frames_decoded);
  out["audio_pcm_samples_decoded"] = Json::Int64(st.audio_pcm_samples_decoded);
  out["audio_pcm_samples_buffered"] = Json::Int64(st.audio_pcm_samples_buffered);
  out["audio_drain_events_total"] = Json::Int64(st.audio_drain_events_total);
  out["audio_pcm_samples_drained_total"] = Json::Int64(st.audio_pcm_samples_drained_total);
  out["audio_pcm_samples_owned"] = Json::Int64(st.audio_pcm_samples_owned);
  out["audio_last_drain_samples"] = Json::Int64(st.audio_last_drain_samples);
  out["audio_process_events_total"] = Json::Int64(st.audio_process_events_total);
  out["audio_pcm_samples_processed_total"] =
    Json::Int64(st.audio_pcm_samples_processed_total);
  out["audio_last_process_samples"] = Json::Int64(st.audio_last_process_samples);
  out["audio_last_peak_abs_pcm16"] = Json::Int64(st.audio_last_peak_abs_pcm16);
  out["audio_last_rms_pcm16"] = Json::Int64(st.audio_last_rms_pcm16);
  out["audio_render_events_total"] = Json::Int64(st.audio_render_events_total);
  out["audio_pcm_samples_rendered_total"] =
    Json::Int64(st.audio_pcm_samples_rendered_total);
  out["audio_render_window_samples"] = Json::Int64(st.audio_render_window_samples);
  out["audio_last_render_samples"] = Json::Int64(st.audio_last_render_samples);
  if (st.audio_last_sample_rate_hz > 0) {
    out["audio_last_sample_rate_hz"] = Json::Int64(st.audio_last_sample_rate_hz);
  }
  if (st.audio_last_channels > 0) {
    out["audio_last_channels"] = Json::Int64(st.audio_last_channels);
  }
  if (st.audio_last_frame_samples_per_channel > 0) {
    out["audio_last_frame_samples_per_channel"] =
      Json::Int64(st.audio_last_frame_samples_per_channel);
  }
  if (!st.audio_last_codec_name.empty()) out["audio_last_codec_name"] = st.audio_last_codec_name;
  if (!st.audio_last_error.empty()) out["audio_last_error"] = st.audio_last_error;
  if (!st.audio_render_wav_path.empty()) out["audio_render_wav_path"] = st.audio_render_wav_path;
  if (!st.audio_render_last_error.empty()) {
    out["audio_render_last_error"] = st.audio_render_last_error;
  }
  if (st.native_media_provider.isObject()) out["native_media_provider"] = st.native_media_provider;
  out["ready"] = st.ready;
  out["running"] = st.running;
  if (!st.stderr_log_path.empty()) out["stderr_log_path"] = st.stderr_log_path;
  if (!st.ready_file_path.empty()) out["ready_file_path"] = st.ready_file_path;
  if (st.running && st.runtime_kind != "builtin") {
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
  const VoicePeerStartPlan& plan
) {
  if (plan.runtime_kind != st.runtime_kind) return false;
  if (plan.runtime_kind == "bundled" || plan.runtime_kind == "external") {
    if (plan.desired_tool_path != st.tool_path) return false;
    if (plan.desired_node_bin != st.node_bin) return false;
  }
  if (!plan.effective_broker_url.empty() && plan.effective_broker_url != st.broker_url) return false;
  if (plan.has_requested_broker_session_id &&
      (!plan.requested_broker_session_id.empty() &&
       (plan.requested_broker_session_id != st.broker_session_id || st.managed_broker_session))) {
    return false;
  }
  if (plan.has_broker_agent_id || plan.has_broker_deployment_id) {
    if (!st.managed_broker_session) return false;
    if (plan.broker_agent_id != st.broker_agent_id) return false;
    if (plan.broker_deployment_id != st.broker_deployment_id) return false;
  }
  if (plan.has_sender_tag && plan.sender_tag != st.sender_tag) return false;
  if (plan.has_deadline_ms && plan.deadline_ms != st.deadline_ms) {
    return false;
  }
  if (plan.has_poll_interval_ms && plan.poll_interval_ms != st.poll_interval_ms) {
    return false;
  }
  if (plan.has_tone_hz && plan.tone_hz != st.tone_hz) {
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
  if (v.isMember("media_engine_kind") && v["media_engine_kind"].isString()) {
    st.media_engine_kind = trim_copy(v["media_engine_kind"].asString());
  }
  if (v.isMember("media_engine_state") && v["media_engine_state"].isString()) {
    st.media_engine_state = trim_copy(v["media_engine_state"].asString());
  }
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
  if (v.isMember("media_state_updated_unix_ms") &&
      (v["media_state_updated_unix_ms"].isInt64() || v["media_state_updated_unix_ms"].isUInt64())) {
    st.media_state_updated_unix_ms = v["media_state_updated_unix_ms"].asInt64();
  }
  if (v.isMember("media_events_total") &&
      (v["media_events_total"].isInt64() || v["media_events_total"].isUInt64())) {
    st.media_events_total = v["media_events_total"].asInt64();
  }
  if (v.isMember("media_remote_offers_seen") &&
      (v["media_remote_offers_seen"].isInt64() || v["media_remote_offers_seen"].isUInt64())) {
    st.media_remote_offers_seen = v["media_remote_offers_seen"].asInt64();
  }
  if (v.isMember("media_answers_sent") &&
      (v["media_answers_sent"].isInt64() || v["media_answers_sent"].isUInt64())) {
    st.media_answers_sent = v["media_answers_sent"].asInt64();
  }
  if (v.isMember("media_remote_candidates_seen") &&
      (v["media_remote_candidates_seen"].isInt64() || v["media_remote_candidates_seen"].isUInt64())) {
    st.media_remote_candidates_seen = v["media_remote_candidates_seen"].asInt64();
  }
  if (v.isMember("media_remote_byes_seen") &&
      (v["media_remote_byes_seen"].isInt64() || v["media_remote_byes_seen"].isUInt64())) {
    st.media_remote_byes_seen = v["media_remote_byes_seen"].asInt64();
  }
  if (v.isMember("media_local_byes_sent") &&
      (v["media_local_byes_sent"].isInt64() || v["media_local_byes_sent"].isUInt64())) {
    st.media_local_byes_sent = v["media_local_byes_sent"].asInt64();
  }
  if (v.isMember("managed_broker_session") && v["managed_broker_session"].isBool()) {
    st.managed_broker_session = v["managed_broker_session"].asBool();
  }
  if (v.isMember("native_media_supported") && v["native_media_supported"].isBool()) {
    st.native_media_supported = v["native_media_supported"].asBool();
  }
  if (v.isMember("native_media_active") && v["native_media_active"].isBool()) {
    st.native_media_active = v["native_media_active"].asBool();
  }
  if (v.isMember("dtls_identity_ready") && v["dtls_identity_ready"].isBool()) {
    st.dtls_identity_ready = v["dtls_identity_ready"].asBool();
  }
  if (v.isMember("dtls_handshake_ready") && v["dtls_handshake_ready"].isBool()) {
    st.dtls_handshake_ready = v["dtls_handshake_ready"].asBool();
  }
  if (v.isMember("dtls_exporter_ready") && v["dtls_exporter_ready"].isBool()) {
    st.dtls_exporter_ready = v["dtls_exporter_ready"].asBool();
  }
  if (v.isMember("srtp_contexts_ready") && v["srtp_contexts_ready"].isBool()) {
    st.srtp_contexts_ready = v["srtp_contexts_ready"].asBool();
  }
  if (v.isMember("srtp_inbound_ready") && v["srtp_inbound_ready"].isBool()) {
    st.srtp_inbound_ready = v["srtp_inbound_ready"].asBool();
  }
  if (v.isMember("srtp_outbound_ready") && v["srtp_outbound_ready"].isBool()) {
    st.srtp_outbound_ready = v["srtp_outbound_ready"].asBool();
  }
  if (v.isMember("dtls_fingerprint_sha256") && v["dtls_fingerprint_sha256"].isString()) {
    st.dtls_fingerprint_sha256 = trim_copy(v["dtls_fingerprint_sha256"].asString());
  }
  if (v.isMember("dtls_setup_role") && v["dtls_setup_role"].isString()) {
    st.dtls_setup_role = trim_copy(v["dtls_setup_role"].asString());
  }
  if (v.isMember("dtls_certificate_subject") && v["dtls_certificate_subject"].isString()) {
    st.dtls_certificate_subject = trim_copy(v["dtls_certificate_subject"].asString());
  }
  if (v.isMember("dtls_handshake_state") && v["dtls_handshake_state"].isString()) {
    st.dtls_handshake_state = trim_copy(v["dtls_handshake_state"].asString());
  }
  if (v.isMember("dtls_selected_srtp_profile") && v["dtls_selected_srtp_profile"].isString()) {
    st.dtls_selected_srtp_profile = trim_copy(v["dtls_selected_srtp_profile"].asString());
  }
  if (v.isMember("srtp_last_error") && v["srtp_last_error"].isString()) {
    st.srtp_last_error = trim_copy(v["srtp_last_error"].asString());
  }
  if (v.isMember("dtls_packets_sent") &&
      (v["dtls_packets_sent"].isInt64() || v["dtls_packets_sent"].isUInt64())) {
    st.dtls_packets_sent = v["dtls_packets_sent"].asInt64();
  }
  if (v.isMember("dtls_packets_received") &&
      (v["dtls_packets_received"].isInt64() || v["dtls_packets_received"].isUInt64())) {
    st.dtls_packets_received = v["dtls_packets_received"].asInt64();
  }
  if (v.isMember("rtp_packets_received") &&
      (v["rtp_packets_received"].isInt64() || v["rtp_packets_received"].isUInt64())) {
    st.rtp_packets_received = v["rtp_packets_received"].asInt64();
  }
  if (v.isMember("rtp_payload_bytes_received") &&
      (v["rtp_payload_bytes_received"].isInt64() || v["rtp_payload_bytes_received"].isUInt64())) {
    st.rtp_payload_bytes_received = v["rtp_payload_bytes_received"].asInt64();
  }
  if (v.isMember("rtp_last_payload_type") &&
      (v["rtp_last_payload_type"].isInt64() || v["rtp_last_payload_type"].isUInt64())) {
    st.rtp_last_payload_type = v["rtp_last_payload_type"].asInt64();
  }
  if (v.isMember("rtp_last_sequence") &&
      (v["rtp_last_sequence"].isInt64() || v["rtp_last_sequence"].isUInt64())) {
    st.rtp_last_sequence = v["rtp_last_sequence"].asInt64();
  }
  if (v.isMember("rtp_last_timestamp") &&
      (v["rtp_last_timestamp"].isInt64() || v["rtp_last_timestamp"].isUInt64())) {
    st.rtp_last_timestamp = v["rtp_last_timestamp"].asInt64();
  }
  if (v.isMember("rtp_last_ssrc") &&
      (v["rtp_last_ssrc"].isInt64() || v["rtp_last_ssrc"].isUInt64())) {
    st.rtp_last_ssrc = v["rtp_last_ssrc"].asInt64();
  }
  if (v.isMember("audio_frames_decoded") &&
      (v["audio_frames_decoded"].isInt64() || v["audio_frames_decoded"].isUInt64())) {
    st.audio_frames_decoded = v["audio_frames_decoded"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_decoded") &&
      (v["audio_pcm_samples_decoded"].isInt64() || v["audio_pcm_samples_decoded"].isUInt64())) {
    st.audio_pcm_samples_decoded = v["audio_pcm_samples_decoded"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_buffered") &&
      (v["audio_pcm_samples_buffered"].isInt64() || v["audio_pcm_samples_buffered"].isUInt64())) {
    st.audio_pcm_samples_buffered = v["audio_pcm_samples_buffered"].asInt64();
  }
  if (v.isMember("audio_drain_events_total") &&
      (v["audio_drain_events_total"].isInt64() || v["audio_drain_events_total"].isUInt64())) {
    st.audio_drain_events_total = v["audio_drain_events_total"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_drained_total") &&
      (v["audio_pcm_samples_drained_total"].isInt64() ||
       v["audio_pcm_samples_drained_total"].isUInt64())) {
    st.audio_pcm_samples_drained_total = v["audio_pcm_samples_drained_total"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_owned") &&
      (v["audio_pcm_samples_owned"].isInt64() || v["audio_pcm_samples_owned"].isUInt64())) {
    st.audio_pcm_samples_owned = v["audio_pcm_samples_owned"].asInt64();
  }
  if (v.isMember("audio_last_drain_samples") &&
      (v["audio_last_drain_samples"].isInt64() || v["audio_last_drain_samples"].isUInt64())) {
    st.audio_last_drain_samples = v["audio_last_drain_samples"].asInt64();
  }
  if (v.isMember("audio_process_events_total") &&
      (v["audio_process_events_total"].isInt64() ||
       v["audio_process_events_total"].isUInt64())) {
    st.audio_process_events_total = v["audio_process_events_total"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_processed_total") &&
      (v["audio_pcm_samples_processed_total"].isInt64() ||
       v["audio_pcm_samples_processed_total"].isUInt64())) {
    st.audio_pcm_samples_processed_total =
      v["audio_pcm_samples_processed_total"].asInt64();
  }
  if (v.isMember("audio_last_process_samples") &&
      (v["audio_last_process_samples"].isInt64() ||
       v["audio_last_process_samples"].isUInt64())) {
    st.audio_last_process_samples = v["audio_last_process_samples"].asInt64();
  }
  if (v.isMember("audio_last_peak_abs_pcm16") &&
      (v["audio_last_peak_abs_pcm16"].isInt64() ||
       v["audio_last_peak_abs_pcm16"].isUInt64())) {
    st.audio_last_peak_abs_pcm16 = v["audio_last_peak_abs_pcm16"].asInt64();
  }
  if (v.isMember("audio_last_rms_pcm16") &&
      (v["audio_last_rms_pcm16"].isInt64() ||
       v["audio_last_rms_pcm16"].isUInt64())) {
    st.audio_last_rms_pcm16 = v["audio_last_rms_pcm16"].asInt64();
  }
  if (v.isMember("audio_render_events_total") &&
      (v["audio_render_events_total"].isInt64() ||
       v["audio_render_events_total"].isUInt64())) {
    st.audio_render_events_total = v["audio_render_events_total"].asInt64();
  }
  if (v.isMember("audio_pcm_samples_rendered_total") &&
      (v["audio_pcm_samples_rendered_total"].isInt64() ||
       v["audio_pcm_samples_rendered_total"].isUInt64())) {
    st.audio_pcm_samples_rendered_total =
      v["audio_pcm_samples_rendered_total"].asInt64();
  }
  if (v.isMember("audio_render_window_samples") &&
      (v["audio_render_window_samples"].isInt64() ||
       v["audio_render_window_samples"].isUInt64())) {
    st.audio_render_window_samples = v["audio_render_window_samples"].asInt64();
  }
  if (v.isMember("audio_last_render_samples") &&
      (v["audio_last_render_samples"].isInt64() ||
       v["audio_last_render_samples"].isUInt64())) {
    st.audio_last_render_samples = v["audio_last_render_samples"].asInt64();
  }
  if (v.isMember("audio_last_sample_rate_hz") &&
      (v["audio_last_sample_rate_hz"].isInt64() || v["audio_last_sample_rate_hz"].isUInt64())) {
    st.audio_last_sample_rate_hz = v["audio_last_sample_rate_hz"].asInt64();
  }
  if (v.isMember("audio_last_channels") &&
      (v["audio_last_channels"].isInt64() || v["audio_last_channels"].isUInt64())) {
    st.audio_last_channels = v["audio_last_channels"].asInt64();
  }
  if (v.isMember("audio_last_frame_samples_per_channel") &&
      (v["audio_last_frame_samples_per_channel"].isInt64() ||
       v["audio_last_frame_samples_per_channel"].isUInt64())) {
    st.audio_last_frame_samples_per_channel =
      v["audio_last_frame_samples_per_channel"].asInt64();
  }
  if (v.isMember("audio_last_codec_name") && v["audio_last_codec_name"].isString()) {
    st.audio_last_codec_name = trim_copy(v["audio_last_codec_name"].asString());
  }
  if (v.isMember("audio_last_error") && v["audio_last_error"].isString()) {
    st.audio_last_error = trim_copy(v["audio_last_error"].asString());
  }
  if (v.isMember("audio_render_wav_path") && v["audio_render_wav_path"].isString()) {
    st.audio_render_wav_path = v["audio_render_wav_path"].asString();
  }
  if (v.isMember("audio_render_last_error") && v["audio_render_last_error"].isString()) {
    st.audio_render_last_error = trim_copy(v["audio_render_last_error"].asString());
  }
  if (v.isMember("native_media_provider") && v["native_media_provider"].isObject()) {
    st.native_media_provider = v["native_media_provider"];
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
  if (trim_copy(st.status_source) == "planned") {
    if (out_err) *out_err = "refusing to persist planned voice runtime preview";
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
  if (trim_copy(st->status_source) == "planned") {
    std::string cerr;
    if (!clear_voice_peer_runtime_record(db, session_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? "failed to clear invalid planned persisted voice runtime record"
          : ("failed to clear invalid planned persisted voice runtime record: " + cerr);
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
  refresh_voice_peer_runtime_backend_state(st.get());
  st->stale_persisted_record = persisted_claimed_running && !st->running;
  if (out_state) *out_state = std::move(st);
  return true;
}

bool recover_voice_peer_runtime_record(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_updates) *out_updates = Json::Value(Json::objectValue);

  bool record_self_healed = false;
  std::shared_ptr<VoicePeerRuntime> st;
  if (!load_voice_peer_runtime_record(db, session_id, &st, &record_self_healed, out_err)) return false;

  if (record_self_healed && out_updates) {
    (*out_updates)["cleanup_on_corrupt_record"] = voice_peer_corrupt_record_cleanup_json(cfg, session_id);
  }
  if (st && st->stale_persisted_record) {
    if (out_updates) {
      (*out_updates)["cleanup_on_stale_record"] =
        cleanup_stale_persisted_voice_peer_runtime(cfg, db, session_id, st->runtime_kind);
    } else {
      (void)cleanup_stale_persisted_voice_peer_runtime(cfg, db, session_id, st->runtime_kind);
    }
    st.reset();
  }
  if (out_state) *out_state = std::move(st);
  return true;
}

Json::Value cleanup_stale_persisted_voice_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  const std::string& runtime_kind
) {
  Json::Value cleanup(Json::objectValue);
  cleanup["persisted_record_cleared"] = clear_voice_peer_runtime_record(db, session_id, nullptr);
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_voice_peer_runtime_backend_artifacts(
        cfg, session_id, runtime_kind, &artifacts_deleted, &aerr)) {
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
  if (remove_voice_peer_runtime_backend_artifacts(
        cfg, session_id, std::string(), &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  return cleanup;
}

}  // namespace agentd
