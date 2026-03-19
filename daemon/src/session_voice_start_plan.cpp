#include "session_voice_start_plan.h"

#include "session_voice_backend_policy.h"
#include "session_voice_broker_client.h"
#include "string_util.h"

namespace agentd {
namespace {

bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

bool is_safe_shellish_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    if (!ok) return false;
  }
  return true;
}

void set_plan_error(VoicePeerPlanError* out_err, int http_status, bool use_json_error_body, const std::string& message) {
  if (!out_err) return;
  out_err->http_status = http_status;
  out_err->use_json_error_body = use_json_error_body;
  out_err->message = message;
}

bool preflight_requested_voice_peer_broker_session(
  VoicePeerStartPlan* plan,
  VoicePeerPlanError* out_err
) {
  if (!plan || plan->requested_broker_session_id.empty()) return true;
  if (plan->requested_broker_session_preflighted) return true;

  bool session_exists = false;
  std::string broker_session_mode;
  std::string err;
  if (!broker_audio_session_exists(
        plan->effective_broker_url,
        plan->broker_token,
        plan->requested_broker_session_id,
        &session_exists,
        &broker_session_mode,
        &err)) {
    set_plan_error(
      out_err,
      500,
      false,
      err.empty() ? "failed to inspect broker audio session" : err);
    return false;
  }
  if (!session_exists) {
    set_plan_error(out_err, 400, true, "broker_session_id not found");
    return false;
  }
  if (!broker_session_mode.empty() && broker_session_mode != "webrtc") {
    set_plan_error(out_err, 400, true, "broker_session_id mode must be webrtc");
    return false;
  }

  plan->requested_broker_session_preflighted = true;
  plan->requested_broker_session_mode = broker_session_mode;
  return true;
}

}  // namespace

bool build_voice_peer_start_plan(
  const DaemonConfig& cfg,
  const Json::Value& body,
  VoicePeerStartPlan* out_plan,
  VoicePeerPlanError* out_err
) {
  if (out_err) *out_err = VoicePeerPlanError{};
  if (!out_plan) {
    set_plan_error(out_err, 500, false, "start plan output unavailable");
    return false;
  }

  VoicePeerStartPlan plan;
  plan.runtime_kind = body.isMember("runtime_kind") && body["runtime_kind"].isString()
    ? lower_copy(trim_copy(body["runtime_kind"].asString()))
    : default_voice_peer_runtime_kind(cfg);
  if (plan.runtime_kind != "external" && plan.runtime_kind != "bundled" && plan.runtime_kind != "builtin") {
    set_plan_error(out_err, 400, true, "runtime_kind must be bundled, external, or builtin");
    return false;
  }

  if (body.isMember("deadline_ms") &&
      (body["deadline_ms"].isInt64() || body["deadline_ms"].isUInt64() || body["deadline_ms"].isInt())) {
    plan.has_deadline_ms = true;
    plan.deadline_ms = body["deadline_ms"].asInt64();
  }
  if (plan.deadline_ms < 1000) plan.deadline_ms = 1000;
  if (plan.deadline_ms > 120000) plan.deadline_ms = 120000;

  if (body.isMember("poll_interval_ms") &&
      (body["poll_interval_ms"].isInt64() || body["poll_interval_ms"].isUInt64() || body["poll_interval_ms"].isInt())) {
    plan.has_poll_interval_ms = true;
    plan.poll_interval_ms = body["poll_interval_ms"].asInt64();
  }
  if (plan.poll_interval_ms < 25) plan.poll_interval_ms = 25;
  if (plan.poll_interval_ms > 5000) plan.poll_interval_ms = 5000;

  if (body.isMember("tone_hz") &&
      (body["tone_hz"].isInt64() || body["tone_hz"].isUInt64() || body["tone_hz"].isInt())) {
    plan.has_tone_hz = true;
    plan.tone_hz = body["tone_hz"].asInt64();
  }
  if (plan.tone_hz < 50) plan.tone_hz = 50;
  if (plan.tone_hz > 4000) plan.tone_hz = 4000;

  if (body.isMember("startup_wait_ms") &&
      (body["startup_wait_ms"].isInt64() || body["startup_wait_ms"].isUInt64() || body["startup_wait_ms"].isInt())) {
    plan.startup_wait_ms = body["startup_wait_ms"].asInt64();
  }
  if (plan.startup_wait_ms < 0) plan.startup_wait_ms = 0;
  if (plan.startup_wait_ms > 10000) plan.startup_wait_ms = 10000;

  plan.request_broker_url =
    body.isMember("broker_url") && body["broker_url"].isString() ? trim_copy(body["broker_url"].asString()) : "";
  plan.requested_broker_session_id =
    body.isMember("broker_session_id") && body["broker_session_id"].isString() ? trim_copy(body["broker_session_id"].asString()) : "";
  plan.broker_agent_id =
    body.isMember("broker_agent_id") && body["broker_agent_id"].isString() ? trim_copy(body["broker_agent_id"].asString()) : "";
  plan.broker_deployment_id =
    body.isMember("broker_deployment_id") && body["broker_deployment_id"].isString()
      ? trim_copy(body["broker_deployment_id"].asString())
      : "";
  plan.sender_tag =
    body.isMember("sender_tag") && body["sender_tag"].isString()
      ? trim_copy(body["sender_tag"].asString())
      : std::string("agentd_runtime_peer");
  if (plan.sender_tag.empty()) plan.sender_tag = "agentd_runtime_peer";

  plan.has_requested_broker_session_id = body.isMember("broker_session_id") && body["broker_session_id"].isString();
  plan.has_broker_agent_id = body.isMember("broker_agent_id") && body["broker_agent_id"].isString();
  plan.has_broker_deployment_id = body.isMember("broker_deployment_id") && body["broker_deployment_id"].isString();
  plan.has_sender_tag = body.isMember("sender_tag") && body["sender_tag"].isString();

  plan.effective_broker_url = effective_voice_broker_url(cfg, plan.request_broker_url);
  if (plan.runtime_kind == "bundled") plan.desired_tool_path = discover_bundled_audio_peer_tool_path(cfg);
  if (plan.runtime_kind == "external") plan.desired_tool_path = trim_copy(cfg.audio_webrtc_peer_tool_path);
  plan.desired_node_bin = trim_copy(
    cfg.audio_webrtc_peer_node_bin.empty() ? std::string("node") : cfg.audio_webrtc_peer_node_bin);

  *out_plan = std::move(plan);
  return true;
}

bool finalize_voice_peer_start_plan_for_launch(
  const DaemonConfig& cfg,
  const std::string& request_broker_token,
  VoicePeerStartPlan* plan,
  VoicePeerPlanError* out_err
) {
  if (out_err) *out_err = VoicePeerPlanError{};
  if (!plan) {
    set_plan_error(out_err, 500, false, "start plan unavailable");
    return false;
  }

  if (!plan->requested_broker_session_id.empty() && !is_safe_shellish_token(plan->requested_broker_session_id, 160)) {
    set_plan_error(out_err, 400, true, "invalid broker_session_id");
    return false;
  }
  if (!plan->broker_agent_id.empty() && !is_safe_shellish_token(plan->broker_agent_id, 160)) {
    set_plan_error(out_err, 400, true, "invalid broker_agent_id");
    return false;
  }
  if (!plan->broker_deployment_id.empty() && !is_safe_shellish_token(plan->broker_deployment_id, 160)) {
    set_plan_error(out_err, 400, true, "invalid broker_deployment_id");
    return false;
  }
  if (!plan->request_broker_url.empty() && !is_safe_printable_field(plan->request_broker_url, 2048)) {
    set_plan_error(out_err, 400, true, "invalid broker_url");
    return false;
  }
  if (!plan->requested_broker_session_id.empty() &&
      (!plan->broker_agent_id.empty() || !plan->broker_deployment_id.empty())) {
    set_plan_error(
      out_err,
      400,
      true,
      "broker_agent_id and broker_deployment_id must be omitted when broker_session_id is provided");
    return false;
  }
  if (plan->effective_broker_url.empty()) {
    set_plan_error(out_err, 400, true, "broker_url required when daemon default not configured");
    return false;
  }
  if (!is_safe_printable_field(plan->effective_broker_url, 2048)) {
    set_plan_error(out_err, 500, false, "invalid configured audio_webrtc_broker_url");
    return false;
  }

  plan->broker_token = effective_voice_broker_token(cfg, request_broker_token);
  std::string validation_err;
  if (!validate_voice_broker_token_if_present(plan->broker_token, &validation_err)) {
    set_plan_error(out_err, 500, false, validation_err);
    return false;
  }
  if (plan->broker_token.empty()) {
    set_plan_error(out_err, 400, true, "broker_token required when daemon default not configured");
    return false;
  }
  if (plan->requested_broker_session_id.empty() && plan->broker_agent_id.empty()) {
    set_plan_error(out_err, 400, true, "broker_agent_id required when broker_session_id omitted");
    return false;
  }
  if (!is_safe_shellish_token(plan->sender_tag, 96)) {
    set_plan_error(out_err, 400, true, "invalid sender_tag");
    return false;
  }

  if (plan->runtime_kind != "builtin") {
    plan->desired_backend_available =
      resolve_voice_peer_backend(
        cfg,
        plan->runtime_kind,
        &plan->resolved_tool_path,
        &plan->resolved_node_bin,
        &plan->desired_backend_err);
    if (!plan->desired_backend_available) {
      set_plan_error(
        out_err,
        500,
        false,
        plan->desired_backend_err.empty() ? "failed to resolve voice peer backend" : plan->desired_backend_err);
      return false;
    }
  }

  if (!preflight_requested_voice_peer_broker_session(plan, out_err)) {
    return false;
  }

  return true;
}

}  // namespace agentd
