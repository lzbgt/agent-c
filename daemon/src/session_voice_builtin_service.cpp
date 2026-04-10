#include "session_voice_builtin_service.h"

#include "session_voice_process_plan.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_runtime_seed.h"
#include "session_voice_signal_client.h"
#include "session_voice_signal_negotiation.h"
#include "session_voice_signal_session.h"
#include "string_util.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

namespace agentd {
namespace {

struct BuiltinVoicePeerService {
  std::string session_id;
  std::string broker_url;
  std::string broker_token;
  std::string broker_session_id;
  std::string sender_tag;
  std::string ready_file_path;
  std::string stdout_log_path;
  std::weak_ptr<VoicePeerRuntime> runtime;
  std::function<void(const VoicePeerRuntime&)> persist_runtime;
  std::mutex mu;
  std::condition_variable cv;
  bool stop_requested = false;
  bool exited = false;
  std::thread worker;
};

std::mutex builtin_voice_peer_services_mu;
std::map<std::string, std::shared_ptr<BuiltinVoicePeerService>> builtin_voice_peer_services;

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::shared_ptr<BuiltinVoicePeerService> lookup_builtin_voice_peer_service(
  const std::string& session_id
) {
  std::lock_guard<std::mutex> lk(builtin_voice_peer_services_mu);
  const auto it = builtin_voice_peer_services.find(trim_copy(session_id));
  return it == builtin_voice_peer_services.end() ? nullptr : it->second;
}

void store_builtin_voice_peer_service(
  const std::shared_ptr<BuiltinVoicePeerService>& service
) {
  if (!service) return;
  std::lock_guard<std::mutex> lk(builtin_voice_peer_services_mu);
  builtin_voice_peer_services[service->session_id] = service;
}

void erase_builtin_voice_peer_service(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(builtin_voice_peer_services_mu);
  builtin_voice_peer_services.erase(trim_copy(session_id));
}

void write_builtin_voice_peer_ready_file(
  const std::string& ready_file_path,
  const std::string& session_id
) {
  if (trim_copy(ready_file_path).empty()) return;
  std::ofstream out(ready_file_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return;
  Json::Value payload(Json::objectValue);
  payload["ok"] = true;
  payload["session_id"] = session_id;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  out << Json::writeString(wb, payload) << "\n";
}

void append_builtin_voice_peer_stdout_line(
  const std::string& stdout_log_path,
  const Json::Value& payload
) {
  if (trim_copy(stdout_log_path).empty() || !payload.isObject()) return;
  std::ofstream out(stdout_log_path, std::ios::binary | std::ios::app);
  if (!out.is_open()) return;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  out << Json::writeString(wb, payload) << "\n";
}

void append_builtin_voice_peer_stderr_line(
  const std::string& stderr_log_path,
  const std::string& line
) {
  if (trim_copy(stderr_log_path).empty()) return;
  std::ofstream out(stderr_log_path, std::ios::binary | std::ios::app);
  if (!out.is_open()) return;
  out << line << "\n";
}

void update_builtin_runtime_last_stdout(
  VoicePeerRuntime* runtime,
  const Json::Value& payload
) {
  if (!runtime || !payload.isObject()) return;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  runtime->last_stdout_json = payload;
  runtime->last_stdout_line = Json::writeString(wb, payload);
  if (payload.isMember("error") && payload["error"].isString()) {
    runtime->last_error = payload["error"].asString();
  }
}

Json::Value make_builtin_voice_peer_event(
  const std::string& event,
  const std::string& session_id
) {
  Json::Value payload(Json::objectValue);
  payload["ok"] = true;
  payload["event"] = event;
  payload["session_id"] = session_id;
  payload["runtime_kind"] = "builtin";
  payload["ts_unix_ms"] = Json::Int64(now_unix_ms());
  return payload;
}

bool builtin_voice_peer_stream_timed_out(const std::string& err) {
  const std::string lowered = lower_copy(trim_copy(err));
  return lowered.find("timeout") != std::string::npos;
}

void reap_builtin_voice_peer_service_if_exited(
  const std::string& session_id
) {
  std::shared_ptr<BuiltinVoicePeerService> service = lookup_builtin_voice_peer_service(session_id);
  if (!service) return;
  {
    std::lock_guard<std::mutex> lk(service->mu);
    if (!service->exited) return;
  }
  if (service->worker.joinable()) service->worker.join();
  erase_builtin_voice_peer_service(session_id);
}

void builtin_voice_peer_service_main(
  const std::shared_ptr<BuiltinVoicePeerService>& service,
  std::mutex& runtime_mu
) {
  std::string terminal_error;
  bool remote_bye_received = false;
  VoiceBrokerSignalSessionState signal_state(service->sender_tag);

  for (;;) {
    {
      std::lock_guard<std::mutex> lk(service->mu);
      if (service->stop_requested) break;
    }

    long http_status = 0;
    std::string stream_err;
    std::string callback_err;
    const bool ok = stream_voice_broker_signal_session(
      service->broker_url,
      service->broker_token,
      service->broker_session_id,
      service->sender_tag,
      1000,
      &signal_state,
      [&](const VoiceBrokerSignalIngress& ingress) {
        if (ingress.kind == VoiceBrokerSignalIngressKind::remote_description) {
          VoiceBrokerSignalRemoteDescriptionReady ready;
          if (!finalize_voice_broker_remote_description_ready(&signal_state, ingress, &ready, &callback_err)) {
            return false;
          }
          const std::string desc_type = lower_copy(trim_copy(ready.description.type));
          if (!desc_type.empty() && desc_type != "offer") {
            callback_err = "expected remote offer, got " + desc_type;
            return false;
          }

          Json::Value seen_offer = make_builtin_voice_peer_event("remote_offer_seen", service->session_id);
          seen_offer["initial_remote_candidate_count"] = Json::UInt64(ready.initial_remote_candidates.size());
          append_builtin_voice_peer_stdout_line(service->stdout_log_path, seen_offer);
          if (auto runtime = service->runtime.lock()) {
            std::lock_guard<std::mutex> lk(runtime_mu);
            update_builtin_runtime_last_stdout(runtime.get(), seen_offer);
          }

          VoiceBrokerSignalDescription answer;
          answer.type = "answer";
          answer.sdp = "stub-answer";
          answer.sender_tag = service->sender_tag;
          if (!send_voice_broker_answer(
                service->broker_url,
                service->broker_token,
                service->broker_session_id,
                answer,
                &callback_err)) {
            return false;
          }

          Json::Value sent_answer = make_builtin_voice_peer_event("stub_answer_sent", service->session_id);
          sent_answer["remote_offer_type"] = ready.description.type;
          append_builtin_voice_peer_stdout_line(service->stdout_log_path, sent_answer);
          if (auto runtime = service->runtime.lock()) {
            std::lock_guard<std::mutex> lk(runtime_mu);
            update_builtin_runtime_last_stdout(runtime.get(), sent_answer);
          }
          return true;
        }

        if (ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_ready) {
          Json::Value candidate = make_builtin_voice_peer_event("remote_candidate_ready", service->session_id);
          candidate["candidate"] = ingress.candidate.candidate;
          append_builtin_voice_peer_stdout_line(service->stdout_log_path, candidate);
          if (auto runtime = service->runtime.lock()) {
            std::lock_guard<std::mutex> lk(runtime_mu);
            update_builtin_runtime_last_stdout(runtime.get(), candidate);
          }
          return true;
        }

        if (ingress.kind == VoiceBrokerSignalIngressKind::remote_bye) {
          remote_bye_received = true;
          Json::Value bye = make_builtin_voice_peer_event("remote_bye", service->session_id);
          if (!trim_copy(ingress.bye.reason).empty()) bye["reason"] = ingress.bye.reason;
          append_builtin_voice_peer_stdout_line(service->stdout_log_path, bye);
          if (auto runtime = service->runtime.lock()) {
            std::lock_guard<std::mutex> lk(runtime_mu);
            update_builtin_runtime_last_stdout(runtime.get(), bye);
          }
          return false;
        }
        return true;
      },
      &http_status,
      &stream_err);

    if (!callback_err.empty()) {
      terminal_error = callback_err;
      break;
    }
    if (remote_bye_received) break;
    if (ok) continue;
    if (builtin_voice_peer_stream_timed_out(stream_err)) continue;
    terminal_error = trim_copy(stream_err).empty()
      ? "builtin voice_webrtc_peer signaling stream failed"
      : stream_err;
    append_builtin_voice_peer_stderr_line(
      service->runtime.lock() ? service->runtime.lock()->stderr_log_path : std::string(),
      terminal_error);
    break;
  }

  const bool stop_requested = [&]() {
    std::lock_guard<std::mutex> lk(service->mu);
    return service->stop_requested;
  }();
  if (stop_requested && !remote_bye_received && signal_state.remote_offer_seen()) {
    VoiceBrokerSignalBye bye;
    bye.reason = "agentd_builtin_stop";
    bye.sender_tag = service->sender_tag;
    std::string send_err;
    (void)send_voice_broker_bye(
      service->broker_url, service->broker_token, service->broker_session_id, bye, &send_err);
    Json::Value sent_bye = make_builtin_voice_peer_event("local_bye_sent", service->session_id);
    sent_bye["reason"] = bye.reason;
    if (!trim_copy(send_err).empty()) sent_bye["warning"] = send_err;
    append_builtin_voice_peer_stdout_line(service->stdout_log_path, sent_bye);
  }

  if (auto runtime = service->runtime.lock()) {
    VoicePeerRuntime persisted_snapshot;
    bool should_persist = false;
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      runtime->running = false;
      if (runtime->ended_unix_ms <= 0) runtime->ended_unix_ms = now_unix_ms();
      runtime->exit_code = terminal_error.empty() ? 0 : 1;
      if (!terminal_error.empty()) runtime->last_error = terminal_error;
      should_persist = !runtime->suppress_persist;
      if (should_persist) persisted_snapshot = *runtime;
    }
    if (should_persist && service->persist_runtime) service->persist_runtime(persisted_snapshot);
  }

  {
    std::lock_guard<std::mutex> lk(service->mu);
    service->exited = true;
  }
  service->cv.notify_all();
}

}  // namespace

bool start_builtin_voice_peer_runtime_service(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerBrokerSessionBinding& binding,
  std::mutex& runtime_mu,
  const std::function<void(const VoicePeerRuntime&)>& persist_runtime,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();

  reap_builtin_voice_peer_service_if_exited(session_id);

  const VoicePeerRuntimeArtifactsPlan artifacts =
    plan_voice_peer_runtime_artifacts(cfg, session_id);
  std::error_code ec;
  const std::filesystem::path runtime_dir(artifacts.runtime_dir);
  std::filesystem::create_directories(runtime_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create builtin voice runtime dir";
    return false;
  }
  std::filesystem::remove(std::filesystem::path(artifacts.ready_file_path), ec);
  ec.clear();
  std::filesystem::remove(std::filesystem::path(artifacts.stdout_log_path), ec);
  ec.clear();
  std::filesystem::remove(std::filesystem::path(artifacts.stderr_log_path), ec);

  write_builtin_voice_peer_ready_file(artifacts.ready_file_path, session_id);

  VoicePeerRuntimeSeed seed;
  seed.runtime_kind = "builtin";
  seed.session_id = trim_copy(session_id);
  seed.broker_session_id = binding.broker_session_id;
  seed.broker_url = start_plan.effective_broker_url;
  seed.managed_broker_session = binding.managed_broker_session;
  seed.broker_agent_id = start_plan.broker_agent_id;
  seed.broker_deployment_id = start_plan.broker_deployment_id;
  seed.sender_tag = start_plan.sender_tag;
  seed.tool_path = "@builtin";
  seed.node_bin = "@builtin";
  seed.ready_file_path = artifacts.ready_file_path;
  seed.stdout_log_path = artifacts.stdout_log_path;
  seed.stderr_log_path = artifacts.stderr_log_path;
  seed.deadline_ms = start_plan.deadline_ms;
  seed.poll_interval_ms = start_plan.poll_interval_ms;
  seed.tone_hz = start_plan.tone_hz;
  seed.ready = true;
  seed.running = true;
  auto runtime = make_voice_peer_runtime_state(seed);

  Json::Value started = make_builtin_voice_peer_event("builtin_runtime_started", session_id);
  started["broker_session_id"] = binding.broker_session_id;
  started["managed_broker_session"] = binding.managed_broker_session;
  append_builtin_voice_peer_stdout_line(artifacts.stdout_log_path, started);
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    update_builtin_runtime_last_stdout(runtime.get(), started);
  }

  auto service = std::make_shared<BuiltinVoicePeerService>();
  service->session_id = trim_copy(session_id);
  service->broker_url = start_plan.effective_broker_url;
  service->broker_token = start_plan.broker_token;
  service->broker_session_id = binding.broker_session_id;
  service->sender_tag = start_plan.sender_tag;
  service->ready_file_path = artifacts.ready_file_path;
  service->stdout_log_path = artifacts.stdout_log_path;
  service->runtime = runtime;
  service->persist_runtime = persist_runtime;
  service->worker = std::thread(
    [service, &runtime_mu]() { builtin_voice_peer_service_main(service, runtime_mu); });
  store_builtin_voice_peer_service(service);

  if (out_state) *out_state = runtime;
  return true;
}

void refresh_builtin_voice_peer_runtime_state(VoicePeerRuntime* st) {
  if (!st) return;
  reap_builtin_voice_peer_service_if_exited(st->session_id);
  const std::shared_ptr<BuiltinVoicePeerService> service =
    lookup_builtin_voice_peer_service(st->session_id);
  if (service) {
    std::lock_guard<std::mutex> lk(service->mu);
    if (!service->exited) return;
  }
  if (st->running) {
    st->running = false;
    if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
    if (trim_copy(st->last_error).empty()) {
      st->last_error = "builtin voice_webrtc_peer runtime exited";
    }
  }
}

bool stop_builtin_voice_peer_runtime_service(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  bool* out_stopped,
  std::string* out_err
) {
  if (out_stopped) *out_stopped = false;
  if (out_err) out_err->clear();
  if (!st) {
    if (out_err) *out_err = "voice peer runtime missing";
    return false;
  }

  reap_builtin_voice_peer_service_if_exited(st->session_id);
  const std::shared_ptr<BuiltinVoicePeerService> service =
    lookup_builtin_voice_peer_service(st->session_id);
  if (!service) {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_builtin_voice_peer_runtime_state(st.get());
    if (out_stopped) *out_stopped = !st->running;
    return true;
  }

  {
    std::lock_guard<std::mutex> lk(service->mu);
    service->stop_requested = true;
  }
  service->cv.notify_all();

  const auto wait_deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
  {
    std::unique_lock<std::mutex> lk(service->mu);
    while (!service->exited) {
      if (timeout_ms <= 0) {
        service->cv.wait(lk);
      } else if (service->cv.wait_until(lk, wait_deadline) == std::cv_status::timeout) {
        if (!service->exited) {
          if (out_err) *out_err = "timed out waiting for builtin voice runtime to stop";
          return false;
        }
      }
    }
  }

  if (service->worker.joinable()) service->worker.join();
  erase_builtin_voice_peer_service(st->session_id);
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_builtin_voice_peer_runtime_state(st.get());
  }
  if (out_stopped) *out_stopped = !st->running;
  return true;
}

}  // namespace agentd
