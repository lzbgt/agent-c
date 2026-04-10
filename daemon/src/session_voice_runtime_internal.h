#pragma once

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#endif

namespace agentd {

struct VoicePeerRuntime {
  std::string runtime_kind = "external";
  std::string status_source = "memory";
  std::string media_engine_kind = "browser_peer";
  std::string media_engine_state = "idle";
  std::string session_id;
  std::string broker_session_id;
  std::string broker_url;
  std::string broker_agent_id;
  std::string broker_deployment_id;
  std::string sender_tag;
  std::string tool_path;
  std::string node_bin;
  std::string ready_file_path;
  std::string stdout_log_path;
  std::string stderr_log_path;
  int64_t started_unix_ms = 0;
  int64_t ended_unix_ms = 0;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  int64_t media_state_updated_unix_ms = 0;
  int64_t media_events_total = 0;
  int64_t media_remote_offers_seen = 0;
  int64_t media_answers_sent = 0;
  int64_t media_remote_candidates_seen = 0;
  int64_t media_remote_byes_seen = 0;
  int64_t media_local_byes_sent = 0;
  bool managed_broker_session = false;
  bool native_media_supported = false;
  bool native_media_active = false;
  bool ready = false;
  bool running = false;
  bool stale_persisted_record = false;
  bool suppress_persist = false;
  int exit_code = 0;
  int exit_signal = 0;
  std::string last_error;
  std::string last_stdout_line;
  Json::Value last_stdout_json = Json::Value(Json::nullValue);
#if defined(_WIN32)
  intptr_t pid = 0;
#else
  pid_t pid = -1;
#endif
};

struct VoicePeerStartupWaitResult {
  bool ready = false;
  bool running = false;
  bool timed_out = false;
};

struct VoicePeerChildLaunchConfig {
  std::string runtime_kind;
  std::string session_id;
  std::string broker_session_id;
  std::string broker_url;
  std::string broker_token;
  std::string broker_agent_id;
  std::string broker_deployment_id;
  std::string sender_tag;
  std::string tool_path;
  std::string node_bin;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  bool managed_broker_session = false;
};

struct VoicePeerRuntimeSeed {
  std::string runtime_kind = "external";
  std::string media_engine_kind = "browser_peer";
  std::string media_engine_state = "idle";
  std::string session_id;
  std::string broker_session_id;
  std::string broker_url;
  std::string broker_agent_id;
  std::string broker_deployment_id;
  std::string sender_tag;
  std::string tool_path;
  std::string node_bin;
  std::string ready_file_path;
  std::string stdout_log_path;
  std::string stderr_log_path;
  int64_t deadline_ms = 15000;
  int64_t poll_interval_ms = 100;
  int64_t tone_hz = 440;
  int64_t media_state_updated_unix_ms = 0;
  int64_t media_events_total = 0;
  int64_t media_remote_offers_seen = 0;
  int64_t media_answers_sent = 0;
  int64_t media_remote_candidates_seen = 0;
  int64_t media_remote_byes_seen = 0;
  int64_t media_local_byes_sent = 0;
  bool managed_broker_session = false;
  bool native_media_supported = false;
  bool native_media_active = false;
  bool ready = false;
  bool running = false;
#if defined(_WIN32)
  intptr_t pid = 0;
#else
  pid_t pid = -1;
#endif
};

}  // namespace agentd
