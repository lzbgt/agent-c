#include "session_voice_signal_client.h"

#include "http_client.h"
#include "string_util.h"

#include "agent/sse_parser.h"

#include <curl/curl.h>

#include <map>

namespace agentd {
namespace {

std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}

struct VoiceBrokerSignalStreamState {
  agent_sse_parser_t parser{};
  VoiceBrokerSignalEventCallback on_event;
  std::string error;
  bool stopped_by_callback = false;
};

size_t voice_broker_signal_stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* st = static_cast<VoiceBrokerSignalStreamState*>(userdata);
  const size_t n = size * nmemb;
  if (!st || !ptr || n == 0) return n;

  agent_sse_event_t events[8];
  size_t event_count = 0;
  const agent_status_t feed_status = agent_sse_parser_feed(&st->parser, ptr, n, events, 8, &event_count);
  if (feed_status != AGENT_OK) {
    st->error = "failed to parse broker signal stream";
    return 0;
  }

  for (size_t i = 0; i < event_count; ++i) {
    const agent_sse_event_t& ev = events[i];
    std::string data;
    if (ev.data.data && ev.data.len > 0) data.assign(ev.data.data, ev.data.len);

    VoiceBrokerSignalEvent parsed;
    const bool ok = parse_voice_broker_signal_event_json(data, &parsed);
    agent_sse_event_free(&events[i]);
    if (!ok) continue;

    if (st->on_event && !st->on_event(parsed)) {
      st->stopped_by_callback = true;
      return 0;
    }
  }
  return n;
}

}  // namespace

bool send_voice_broker_signal(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& type,
  const Json::Value& payload,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  Json::Value body(Json::objectValue);
  body["type"] = type;
  if (!payload.isNull()) body["payload"] = payload;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string body_str = Json::writeString(wb, body);

  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/json";
  headers["Authorization"] = std::string("Bearer ") + token;

  const std::string send_url =
    join_base_path(broker_url, "/v1/audio/sessions/" + session_id + "/signal");
  const HttpClientResult r = http_request(
    send_url, "POST", headers, body_str, 5000, 256 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_err) {
      if (!r.error.empty()) *out_err = r.error;
      else *out_err = "http status " + std::to_string((int)r.http_status);
    }
    return false;
  }
  return true;
}

bool send_voice_broker_answer(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalDescription& answer,
  std::string* out_err
) {
  return send_voice_broker_signal(
    broker_url, token, session_id, "answer", make_voice_broker_description_payload(answer), out_err);
}

bool send_voice_broker_candidate(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalCandidate& candidate,
  std::string* out_err
) {
  return send_voice_broker_signal(
    broker_url, token, session_id, "candidate", make_voice_broker_candidate_payload(candidate), out_err);
}

bool send_voice_broker_bye(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalBye& bye,
  std::string* out_err
) {
  return send_voice_broker_signal(
    broker_url, token, session_id, "bye", make_voice_broker_bye_payload(bye), out_err);
}

bool stream_voice_broker_signal_events(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  const VoiceBrokerSignalEventCallback& on_event,
  long* out_http_status,
  std::string* out_err
) {
  if (out_http_status) *out_http_status = 0;
  if (out_err) out_err->clear();

  CURL* curl = curl_easy_init();
  if (!curl) {
    if (out_err) *out_err = "curl_easy_init failed";
    return false;
  }

  VoiceBrokerSignalStreamState st;
  agent_sse_parser_init(&st.parser);
  st.on_event = on_event;

  const std::string stream_url =
    join_base_path(broker_url, "/v1/audio/sessions/" + session_id + "/signal/stream");
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
  const std::string auth = std::string("Authorization: Bearer ") + token;
  hdrs = curl_slist_append(hdrs, auth.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, stream_url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, voice_broker_signal_stream_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);

  const CURLcode rc = curl_easy_perform(curl);
  long http_status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  if (out_http_status) *out_http_status = http_status;

  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  agent_sse_parser_free(&st.parser);

  if (st.stopped_by_callback) return true;
  if (rc != CURLE_OK) {
    if (out_err) {
      if (!st.error.empty()) *out_err = st.error;
      else *out_err = curl_easy_strerror(rc);
    }
    return false;
  }
  return true;
}

bool stream_voice_broker_signal_session(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& self_sender_tag,
  int64_t timeout_ms,
  VoiceBrokerSignalSessionState* io_state,
  const VoiceBrokerSignalIngressCallback& on_ingress,
  long* out_http_status,
  std::string* out_err
) {
  VoiceBrokerSignalSessionState local_state(self_sender_tag);
  VoiceBrokerSignalSessionState* state = io_state ? io_state : &local_state;
  bool callback_failed = false;
  std::string callback_error;
  const bool ok = stream_voice_broker_signal_events(
    broker_url,
    token,
    session_id,
    timeout_ms,
    [&](const VoiceBrokerSignalEvent& ev) {
      VoiceBrokerSignalIngress ingress;
      std::string err;
      if (!state->ingest_event(ev, &ingress, &err)) {
        callback_failed = true;
        callback_error = err.empty() ? "failed to ingest broker signal event" : err;
        return false;
      }
      if (ingress.kind == VoiceBrokerSignalIngressKind::ignored_self ||
          ingress.kind == VoiceBrokerSignalIngressKind::ignored_unknown ||
          ingress.kind == VoiceBrokerSignalIngressKind::ignored_relay_candidate) {
        return true;
      }
      return on_ingress ? on_ingress(ingress) : true;
    },
    out_http_status,
    out_err);
  if (callback_failed) {
    if (out_err) *out_err = callback_error;
    return false;
  }
  return ok;
}

bool wait_for_voice_broker_signal_type(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& expected_type,
  int64_t timeout_ms,
  VoiceBrokerSignalEvent* out_event,
  long* out_http_status,
  std::string* out_err
) {
  if (out_event) *out_event = VoiceBrokerSignalEvent{};
  const std::string expected = trim_copy(expected_type);
  bool matched = false;
  VoiceBrokerSignalEvent found;
  const bool ok = stream_voice_broker_signal_events(
    broker_url,
    token,
    session_id,
    timeout_ms,
    [&](const VoiceBrokerSignalEvent& ev) {
      if (trim_copy(ev.type) != expected) return true;
      matched = true;
      found = ev;
      return false;
    },
    out_http_status,
    out_err);
  if (!ok) return false;
  if (!matched) {
    if (out_err && out_err->empty()) {
      *out_err = expected.empty() ? "signal not received before stream ended"
                                  : ("signal type '" + expected + "' not received before stream ended");
    }
    return false;
  }
  if (out_event) *out_event = std::move(found);
  return true;
}

bool wait_for_voice_broker_signal_remote_description(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& self_sender_tag,
  int64_t timeout_ms,
  VoiceBrokerSignalDescription* out_desc,
  long* out_http_status,
  std::string* out_err
) {
  if (out_desc) *out_desc = VoiceBrokerSignalDescription{};
  VoiceBrokerSignalRemoteDescriptionReady ready;
  if (!wait_for_voice_broker_signal_remote_description_ready(
        broker_url,
        token,
        session_id,
        self_sender_tag,
        timeout_ms,
        &ready,
        out_http_status,
        out_err)) {
    return false;
  }
  if (out_desc) *out_desc = std::move(ready.description);
  return true;
}

bool wait_for_voice_broker_signal_session_ingress_kind(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  VoiceBrokerSignalSessionState* io_state,
  VoiceBrokerSignalIngressKind expected_kind,
  VoiceBrokerSignalIngress* out_ingress,
  long* out_http_status,
  std::string* out_err
) {
  if (out_ingress) *out_ingress = VoiceBrokerSignalIngress{};
  if (!io_state) {
    if (out_err) *out_err = "missing signal session state";
    return false;
  }

  bool matched = false;
  VoiceBrokerSignalIngress found;
  const bool ok = stream_voice_broker_signal_session(
    broker_url,
    token,
    session_id,
    io_state->self_sender_tag(),
    timeout_ms,
    io_state,
    [&](const VoiceBrokerSignalIngress& ingress) {
      if (ingress.kind != expected_kind) return true;
      matched = true;
      found = ingress;
      return false;
    },
    out_http_status,
    out_err);
  if (!ok) return false;
  if (!matched) {
    if (out_err && out_err->empty()) {
      *out_err = "expected signal ingress not received before stream ended";
    }
    return false;
  }
  if (out_ingress) *out_ingress = std::move(found);
  return true;
}

bool wait_for_voice_broker_signal_remote_candidate_ready(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  VoiceBrokerSignalSessionState* io_state,
  VoiceBrokerSignalCandidate* out_candidate,
  long* out_http_status,
  std::string* out_err
) {
  if (out_candidate) *out_candidate = VoiceBrokerSignalCandidate{};
  VoiceBrokerSignalIngress ingress;
  if (!wait_for_voice_broker_signal_session_ingress_kind(
        broker_url,
        token,
        session_id,
        timeout_ms,
        io_state,
        VoiceBrokerSignalIngressKind::remote_candidate_ready,
        &ingress,
        out_http_status,
        out_err)) {
    return false;
  }
  if (out_candidate) *out_candidate = std::move(ingress.candidate);
  return true;
}

bool wait_for_voice_broker_signal_remote_bye(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  VoiceBrokerSignalSessionState* io_state,
  VoiceBrokerSignalBye* out_bye,
  long* out_http_status,
  std::string* out_err
) {
  if (out_bye) *out_bye = VoiceBrokerSignalBye{};
  VoiceBrokerSignalIngress ingress;
  if (!wait_for_voice_broker_signal_session_ingress_kind(
        broker_url,
        token,
        session_id,
        timeout_ms,
        io_state,
        VoiceBrokerSignalIngressKind::remote_bye,
        &ingress,
        out_http_status,
        out_err)) {
    return false;
  }
  if (out_bye) *out_bye = std::move(ingress.bye);
  return true;
}

bool wait_for_voice_broker_signal_remote_description_ready_with_state(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  int64_t timeout_ms,
  VoiceBrokerSignalSessionState* io_state,
  VoiceBrokerSignalRemoteDescriptionReady* out_ready,
  long* out_http_status,
  std::string* out_err
) {
  if (out_ready) *out_ready = VoiceBrokerSignalRemoteDescriptionReady{};
  if (!io_state) {
    if (out_err) *out_err = "missing signal session state";
    return false;
  }

  bool matched = false;
  VoiceBrokerSignalRemoteDescriptionReady found;
  const bool ok = stream_voice_broker_signal_session(
    broker_url,
    token,
    session_id,
    io_state->self_sender_tag(),
    timeout_ms,
    io_state,
    [&](const VoiceBrokerSignalIngress& ingress) {
      if (ingress.kind != VoiceBrokerSignalIngressKind::remote_description) return true;
      std::string err;
      if (!finalize_voice_broker_remote_description_ready(io_state, ingress, &found, &err)) {
        if (out_err) *out_err = err.empty() ? "failed to finalize remote description" : err;
        return false;
      }
      matched = true;
      return false;
    },
    out_http_status,
    out_err);
  if (!ok) return false;
  if (!matched) {
    if (out_err && out_err->empty()) *out_err = "remote description not received before stream ended";
    return false;
  }
  if (out_ready) *out_ready = std::move(found);
  return true;
}

bool wait_for_voice_broker_signal_remote_description_ready(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const std::string& self_sender_tag,
  int64_t timeout_ms,
  VoiceBrokerSignalRemoteDescriptionReady* out_ready,
  long* out_http_status,
  std::string* out_err
) {
  if (out_ready) *out_ready = VoiceBrokerSignalRemoteDescriptionReady{};
  VoiceBrokerSignalSessionState state(self_sender_tag);
  return wait_for_voice_broker_signal_remote_description_ready_with_state(
    broker_url, token, session_id, timeout_ms, &state, out_ready, out_http_status, out_err);
}

}  // namespace agentd
