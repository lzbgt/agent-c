#include "session_voice_builtin_service.h"

#include "session_voice_audio_process.h"
#include "session_voice_builtin_media_engine.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_plan.h"
#include "session_voice_runtime_seed.h"
#include "session_voice_signal_client.h"
#include "session_voice_signal_negotiation.h"
#include "session_voice_signal_session.h"
#include "string_util.h"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <vector>

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
  std::unique_ptr<VoicePeerBuiltinMediaEngine> media_engine;
  std::function<void(const VoicePeerRuntime&)> persist_runtime;
  std::deque<int16_t> owned_audio_pcm;
  int64_t owned_audio_drain_events_total = 0;
  int64_t owned_audio_pcm_samples_drained_total = 0;
  int64_t owned_audio_process_events_total = 0;
  int64_t owned_audio_pcm_samples_processed_total = 0;
  std::mutex mu;
  std::condition_variable cv;
  bool stop_requested = false;
  bool exited = false;
  std::thread worker;
};

std::mutex builtin_voice_peer_services_mu;
std::map<std::string, std::shared_ptr<BuiltinVoicePeerService>> builtin_voice_peer_services;

constexpr size_t kBuiltinVoicePeerOwnedAudioCapacitySamples = 48000 * 2 * 2;
constexpr size_t kBuiltinVoicePeerOwnedAudioProcessSamples = 4096;

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

void update_builtin_runtime_last_stdout(
  VoicePeerRuntime* runtime,
  const Json::Value& payload
);

Json::Value make_builtin_voice_peer_event(
  const std::string& event,
  const std::string& session_id
);

void append_builtin_voice_peer_event_and_snapshot(
  const std::string& stdout_log_path,
  const std::string& session_id,
  const Json::Value& payload,
  const std::weak_ptr<VoicePeerRuntime>& runtime_weak,
  std::mutex& runtime_mu
) {
  if (!payload.isObject()) return;
  Json::Value normalized = payload;
  if (!normalized.isMember("ok")) normalized["ok"] = true;
  if (!normalized.isMember("event")) normalized["event"] = "builtin_runtime_event";
  if (!normalized.isMember("session_id")) normalized["session_id"] = session_id;
  if (!normalized.isMember("runtime_kind")) normalized["runtime_kind"] = "builtin";
  if (!normalized.isMember("ts_unix_ms")) normalized["ts_unix_ms"] = Json::Int64(now_unix_ms());
  append_builtin_voice_peer_stdout_line(stdout_log_path, normalized);
  if (auto runtime = runtime_weak.lock()) {
    std::lock_guard<std::mutex> lk(runtime_mu);
    update_builtin_runtime_last_stdout(runtime.get(), normalized);
    note_voice_peer_media_engine_event(runtime.get(), normalized);
  }
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

bool drain_builtin_voice_peer_media_engine_events(
  const std::shared_ptr<BuiltinVoicePeerService>& service,
  std::mutex& runtime_mu,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!service || !service->media_engine) return true;
  for (int i = 0; i < 32; ++i) {
    Json::Value event(Json::nullValue);
    std::string poll_err;
    if (!service->media_engine->poll_status(&event, &poll_err)) {
      if (out_err) {
        *out_err = trim_copy(poll_err).empty()
          ? "builtin media engine poll_status failed"
          : poll_err;
      }
      return false;
    }
    if (!event.isObject()) return true;
    append_builtin_voice_peer_event_and_snapshot(
      service->stdout_log_path, service->session_id, event, service->runtime, runtime_mu);
  }
  if (out_err) *out_err = "builtin media engine poll_status exceeded drain limit";
  return false;
}

bool drain_builtin_voice_peer_media_engine_audio(
  const std::shared_ptr<BuiltinVoicePeerService>& service,
  std::mutex& runtime_mu,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!service || !service->media_engine) return true;
  for (int i = 0; i < 32; ++i) {
    VoicePeerBuiltinAudioChunk chunk;
    Json::Value event(Json::nullValue);
    std::string drain_err;
    if (!service->media_engine->drain_audio(&chunk, &event, &drain_err)) {
      if (out_err) {
        *out_err = trim_copy(drain_err).empty()
          ? "builtin media engine drain_audio failed"
          : drain_err;
      }
      return false;
    }
    if (chunk.pcm_samples.empty() && !event.isObject()) return true;

    Json::Value payload = event.isObject()
      ? event
      : make_builtin_voice_peer_event("audio_chunk_drained", service->session_id);

    if (!chunk.pcm_samples.empty()) {
      int64_t drain_events_total = 0;
      int64_t pcm_samples_drained_total = 0;
      int64_t pcm_samples_owned = 0;
      {
        std::lock_guard<std::mutex> lk(service->mu);
        service->owned_audio_drain_events_total += 1;
        service->owned_audio_pcm_samples_drained_total +=
          static_cast<int64_t>(chunk.pcm_samples.size());
        for (const int16_t sample : chunk.pcm_samples) {
          service->owned_audio_pcm.push_back(sample);
        }
        while (service->owned_audio_pcm.size() > kBuiltinVoicePeerOwnedAudioCapacitySamples) {
          service->owned_audio_pcm.pop_front();
        }
        drain_events_total = service->owned_audio_drain_events_total;
        pcm_samples_drained_total = service->owned_audio_pcm_samples_drained_total;
        pcm_samples_owned = static_cast<int64_t>(service->owned_audio_pcm.size());
      }
      payload["event"] = "audio_chunk_drained";
      payload["audio_drain_events_total"] = Json::Int64(drain_events_total);
      payload["audio_pcm_samples_drained_total"] = Json::Int64(pcm_samples_drained_total);
      payload["audio_pcm_samples_owned"] = Json::Int64(pcm_samples_owned);
      payload["audio_last_drain_samples"] =
        Json::Int64(static_cast<int64_t>(chunk.pcm_samples.size()));
      if (!payload.isMember("audio_last_sample_rate_hz") && chunk.sample_rate_hz > 0) {
        payload["audio_last_sample_rate_hz"] = chunk.sample_rate_hz;
      }
      if (!payload.isMember("audio_last_channels") && chunk.channels > 0) {
        payload["audio_last_channels"] = chunk.channels;
      }
      if (!payload.isMember("audio_last_frame_samples_per_channel") &&
          chunk.frame_samples_per_channel > 0) {
        payload["audio_last_frame_samples_per_channel"] =
          chunk.frame_samples_per_channel;
      }
      if (!payload.isMember("audio_last_codec_name") && !trim_copy(chunk.codec_name).empty()) {
        payload["audio_last_codec_name"] = chunk.codec_name;
      }
      if (!payload.isMember("native_media_active")) payload["native_media_active"] = true;
      if (!payload.isMember("media_engine_state")) payload["media_engine_state"] = "media_active";
    }

    append_builtin_voice_peer_event_and_snapshot(
      service->stdout_log_path, service->session_id, payload, service->runtime, runtime_mu);
  }
  if (out_err) *out_err = "builtin media engine drain_audio exceeded drain limit";
  return false;
}

bool process_builtin_voice_peer_owned_audio(
  const std::shared_ptr<BuiltinVoicePeerService>& service,
  std::mutex& runtime_mu,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!service) return true;

  for (int i = 0; i < 32; ++i) {
    std::vector<int16_t> samples;
    int64_t process_events_total = 0;
    int64_t pcm_samples_processed_total = 0;
    int64_t pcm_samples_owned = 0;
    {
      std::lock_guard<std::mutex> lk(service->mu);
      if (service->owned_audio_pcm.empty()) return true;

      const size_t process_samples =
        std::min(kBuiltinVoicePeerOwnedAudioProcessSamples, service->owned_audio_pcm.size());
      samples.reserve(process_samples);
      for (size_t j = 0; j < process_samples; ++j) {
        samples.push_back(service->owned_audio_pcm.front());
        service->owned_audio_pcm.pop_front();
      }
      service->owned_audio_process_events_total += 1;
      service->owned_audio_pcm_samples_processed_total +=
        static_cast<int64_t>(samples.size());
      process_events_total = service->owned_audio_process_events_total;
      pcm_samples_processed_total = service->owned_audio_pcm_samples_processed_total;
      pcm_samples_owned = static_cast<int64_t>(service->owned_audio_pcm.size());
    }

    Pcm16AudioProcessSummary summary;
    if (!summarize_pcm16_audio(samples.data(), samples.size(), &summary)) {
      if (out_err) *out_err = "failed to summarize builtin owned audio";
      return false;
    }

    Json::Value payload = make_builtin_voice_peer_event("audio_chunk_processed", service->session_id);
    payload["media_engine_state"] = "media_active";
    payload["native_media_active"] = true;
    payload["audio_process_events_total"] = Json::Int64(process_events_total);
    payload["audio_pcm_samples_processed_total"] =
      Json::Int64(pcm_samples_processed_total);
    payload["audio_pcm_samples_owned"] = Json::Int64(pcm_samples_owned);
    payload["audio_last_process_samples"] =
      Json::Int64(static_cast<int64_t>(summary.sample_count));
    payload["audio_last_peak_abs_pcm16"] = summary.peak_abs_pcm16;
    payload["audio_last_rms_pcm16"] = summary.rms_pcm16;
    append_builtin_voice_peer_event_and_snapshot(
      service->stdout_log_path, service->session_id, payload, service->runtime, runtime_mu);
  }

  return true;
}

bool drain_builtin_voice_peer_media_engine_outputs(
  const std::shared_ptr<BuiltinVoicePeerService>& service,
  std::mutex& runtime_mu,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  std::string err;
  if (!drain_builtin_voice_peer_media_engine_events(service, runtime_mu, &err)) {
    if (out_err) *out_err = err;
    return false;
  }
  if (!drain_builtin_voice_peer_media_engine_audio(service, runtime_mu, &err)) {
    if (out_err) *out_err = err;
    return false;
  }
  if (!process_builtin_voice_peer_owned_audio(service, runtime_mu, &err)) {
    if (out_err) *out_err = err;
    return false;
  }
  return true;
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
          seen_offer["media_engine_state"] = "signaling_ready";
          append_builtin_voice_peer_event_and_snapshot(
            service->stdout_log_path, service->session_id, seen_offer, service->runtime, runtime_mu);

          VoiceBrokerSignalDescription answer;
          Json::Value answer_ready(Json::nullValue);
          if (!service->media_engine ||
              !service->media_engine->handle_remote_description(
                ready, &answer, &answer_ready, &callback_err)) {
            if (trim_copy(callback_err).empty()) {
              callback_err = "builtin media engine failed to process remote description";
            }
            return false;
          }
          if (answer_ready.isObject()) {
            append_builtin_voice_peer_event_and_snapshot(
              service->stdout_log_path, service->session_id, answer_ready, service->runtime, runtime_mu);
          }
          if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &callback_err)) {
            return false;
          }

          answer.sender_tag = service->sender_tag;
          if (!send_voice_broker_answer(
                service->broker_url,
                service->broker_token,
                service->broker_session_id,
                answer,
                &callback_err)) {
            return false;
          }

          Json::Value sent_answer = make_builtin_voice_peer_event("answer_sent", service->session_id);
          sent_answer["remote_offer_type"] = ready.description.type;
          if (service->media_engine) {
            const VoicePeerMediaEngineInfo info = service->media_engine->info();
            sent_answer["media_engine_kind"] = info.media_engine_kind;
            sent_answer["media_engine_state"] = "signaling_active";
            sent_answer["native_media_supported"] = info.native_media_supported;
            sent_answer["native_media_active"] = info.native_media_active;
            const Json::Value provider = voice_peer_media_engine_provider_json(info, true);
            if (provider.isObject()) sent_answer["native_media_provider"] = provider;
          }
          append_builtin_voice_peer_event_and_snapshot(
            service->stdout_log_path, service->session_id, sent_answer, service->runtime, runtime_mu);
          if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &callback_err)) {
            return false;
          }
          return true;
        }

        if (ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_ready) {
          Json::Value candidate(Json::nullValue);
          if (!service->media_engine ||
              !service->media_engine->handle_remote_candidate(ingress, &candidate, &callback_err)) {
            if (trim_copy(callback_err).empty()) {
              callback_err = "builtin media engine failed to process remote candidate";
            }
            return false;
          }
          append_builtin_voice_peer_event_and_snapshot(
            service->stdout_log_path, service->session_id, candidate, service->runtime, runtime_mu);
          if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &callback_err)) {
            return false;
          }
          return true;
        }

        if (ingress.kind == VoiceBrokerSignalIngressKind::remote_bye) {
          remote_bye_received = true;
          Json::Value bye(Json::nullValue);
          if (service->media_engine) {
            service->media_engine->handle_remote_bye(ingress, &bye);
          }
          append_builtin_voice_peer_event_and_snapshot(
            service->stdout_log_path, service->session_id, bye, service->runtime, runtime_mu);
          if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &callback_err)) {
            return false;
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
    if (builtin_voice_peer_stream_timed_out(stream_err)) {
      std::string poll_err;
      if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &poll_err)) {
        terminal_error = trim_copy(poll_err).empty()
          ? "builtin media engine poll_status failed"
          : poll_err;
        break;
      }
      continue;
    }
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
    Json::Value sent_bye(Json::nullValue);
    if (service->media_engine) service->media_engine->handle_local_shutdown(&sent_bye);
    if (!sent_bye.isObject()) {
      sent_bye = make_builtin_voice_peer_event("local_bye_sent", service->session_id);
      sent_bye["reason"] = bye.reason;
    }
    if (!trim_copy(send_err).empty()) sent_bye["warning"] = send_err;
    append_builtin_voice_peer_event_and_snapshot(
      service->stdout_log_path, service->session_id, sent_bye, service->runtime, runtime_mu);
    std::string drain_err;
    (void)drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &drain_err);
  }

  Json::Value terminal_event = make_builtin_voice_peer_event(
    terminal_error.empty() ? "builtin_runtime_stopped" : "builtin_runtime_failed",
    service->session_id);
  terminal_event["media_engine_state"] = terminal_error.empty() ? "stopped" : "failed";
  if (!terminal_error.empty()) terminal_event["error"] = terminal_error;
  if (remote_bye_received) terminal_event["remote_bye_received"] = true;
  if (stop_requested) terminal_event["stop_requested"] = true;
  append_builtin_voice_peer_event_and_snapshot(
    service->stdout_log_path, service->session_id, terminal_event, service->runtime, runtime_mu);

  if (auto runtime = service->runtime.lock()) {
    VoicePeerRuntime persisted_snapshot;
    bool should_persist = false;
    {
      std::lock_guard<std::mutex> lk(runtime_mu);
      runtime->running = false;
      if (runtime->ended_unix_ms <= 0) runtime->ended_unix_ms = now_unix_ms();
      set_voice_peer_media_engine_state(
        runtime.get(), terminal_error.empty() ? "stopped" : "failed", runtime->ended_unix_ms);
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

  std::string media_engine_err;
  std::unique_ptr<VoicePeerBuiltinMediaEngine> media_engine =
    make_builtin_voice_peer_media_engine(cfg, &media_engine_err);
  if (!media_engine) {
    if (out_err) {
      *out_err = trim_copy(media_engine_err).empty()
        ? "builtin media engine unavailable"
        : media_engine_err;
    }
    return false;
  }

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
  apply_voice_peer_media_engine_info(media_engine->info(), &seed);
  set_voice_peer_media_engine_state(&seed, "starting", 0);
  seed.ready = true;
  seed.running = true;
  auto runtime = make_voice_peer_runtime_state(seed);

  Json::Value init_event(Json::nullValue);
  std::string init_err;
  if (!media_engine->initialize(runtime.get(), &init_event, &init_err)) {
    if (out_err) {
      *out_err = trim_copy(init_err).empty()
        ? "failed to initialize builtin media engine"
        : init_err;
    }
    return false;
  }
  if (init_event.isObject()) {
    append_builtin_voice_peer_event_and_snapshot(
      artifacts.stdout_log_path, session_id, init_event, runtime, runtime_mu);
  }

  Json::Value started = make_builtin_voice_peer_event("builtin_runtime_started", session_id);
  started["broker_session_id"] = binding.broker_session_id;
  started["managed_broker_session"] = binding.managed_broker_session;
  started["media_engine_kind"] = runtime->media_engine_kind;
  started["media_engine_state"] = runtime->media_engine_state;
  started["native_media_supported"] = runtime->native_media_supported;
  started["native_media_active"] = runtime->native_media_active;
  if (runtime->native_media_provider.isObject()) {
    started["native_media_provider"] = runtime->native_media_provider;
  }
  append_builtin_voice_peer_event_and_snapshot(
    artifacts.stdout_log_path, session_id, started, runtime, runtime_mu);

  auto service = std::make_shared<BuiltinVoicePeerService>();
  service->session_id = trim_copy(session_id);
  service->broker_url = start_plan.effective_broker_url;
  service->broker_token = start_plan.broker_token;
  service->broker_session_id = binding.broker_session_id;
  service->sender_tag = start_plan.sender_tag;
  service->ready_file_path = artifacts.ready_file_path;
  service->stdout_log_path = artifacts.stdout_log_path;
  service->runtime = runtime;
  service->media_engine = std::move(media_engine);
  service->persist_runtime = persist_runtime;
  std::string poll_err;
  if (!drain_builtin_voice_peer_media_engine_outputs(service, runtime_mu, &poll_err)) {
    if (out_err) {
      *out_err = trim_copy(poll_err).empty()
        ? "failed to drain builtin media engine startup output"
        : poll_err;
    }
    return false;
  }
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
    set_voice_peer_media_engine_state(st, "failed", st->ended_unix_ms);
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
