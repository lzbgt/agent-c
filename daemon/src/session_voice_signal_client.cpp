#include "session_voice_signal_client.h"

#include "http_client.h"
#include "string_util.h"

#include "agent/sse_parser.h"

#include <curl/curl.h>

#include <map>
#include <memory>

namespace agentd {
namespace {

std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}

bool parse_voice_broker_signal_event_json(const std::string& raw, VoiceBrokerSignalEvent* out_event) {
  if (!out_event) return false;
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  std::string errs;
  Json::Value root(Json::nullValue);
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs)) return false;
  if (!root.isObject()) return false;

  VoiceBrokerSignalEvent ev;
  ev.raw = root;
  if (root.isMember("type") && root["type"].isString()) ev.type = trim_copy(root["type"].asString());
  if (root.isMember("payload") && root["payload"].isObject()) ev.payload = root["payload"];
  else ev.payload = Json::Value(Json::objectValue);
  if (root.isMember("from") && root["from"].isString()) ev.from = trim_copy(root["from"].asString());
  if (root.isMember("ts_unix_ms") && (root["ts_unix_ms"].isInt64() || root["ts_unix_ms"].isUInt64())) {
    ev.ts_unix_ms = root["ts_unix_ms"].asInt64();
  }
  *out_event = std::move(ev);
  return true;
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

}  // namespace agentd
