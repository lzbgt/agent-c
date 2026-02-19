#include "http_client.h"

#include <curl/curl.h>
#include <json/json.h>

#include <cctype>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace agentd;

struct Options {
  std::string broker_url;
  std::string token;
  std::string session_id;
  int64_t stream_timeout_ms = 15000;
};

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_audio_signal_loopback")
    << " --broker-url <url> --token <token> --session-id <id> [--stream-timeout-ms <ms>]\n";
}

static bool parse_args(int argc, char** argv, Options* out) {
  if (!out) return false;
  Options opt;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--broker-url" && i + 1 < argc) {
      opt.broker_url = argv[++i];
    } else if (a == "--token" && i + 1 < argc) {
      opt.token = argv[++i];
    } else if (a == "--session-id" && i + 1 < argc) {
      opt.session_id = argv[++i];
    } else if (a == "--stream-timeout-ms" && i + 1 < argc) {
      opt.stream_timeout_ms = std::stoll(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      return false;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      print_usage(argv[0]);
      return false;
    }
  }
  if (opt.broker_url.empty() || opt.token.empty() || opt.session_id.empty()) {
    print_usage(argv[0]);
    return false;
  }
  *out = opt;
  return true;
}

struct SseState {
  std::string buffer;
  bool got_offer = false;
  Json::Value offer_payload;
  std::string error;
};

static bool parse_offer(const std::string& data, SseState* st) {
  if (!st) return false;
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  std::string errs;
  Json::Value root;
  const auto raw = trim_copy(data);
  if (raw.empty()) return false;
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs)) {
    return false;
  }
  if (!root.isObject()) return false;
  const auto type = root.isMember("type") && root["type"].isString() ? root["type"].asString() : "";
  if (type != "offer") return false;
  st->offer_payload = root.isMember("payload") ? root["payload"] : Json::Value(Json::objectValue);
  st->got_offer = true;
  return true;
}

static size_t sse_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* st = static_cast<SseState*>(userdata);
  const size_t n = size * nmemb;
  if (!st || !ptr || n == 0) return n;
  st->buffer.append(ptr, ptr + n);

  size_t pos = 0;
  while (true) {
    const size_t nl = st->buffer.find('\n', pos);
    if (nl == std::string::npos) {
      st->buffer.erase(0, pos);
      break;
    }
    std::string line = st->buffer.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = nl + 1;

    if (line.rfind("data:", 0) == 0) {
      std::string data = line.substr(5);
      if (!data.empty() && data.front() == ' ') data.erase(data.begin());
      if (parse_offer(data, st)) {
        return 0; // abort stream once we have an offer
      }
    }
  }
  return n;
}

static bool read_offer_from_stream(const Options& opt, SseState* st, long* http_status_out) {
  if (!st) return false;
  CURL* curl = curl_easy_init();
  if (!curl) {
    st->error = "curl_easy_init failed";
    return false;
  }

  const std::string stream_url = join_base_path(opt.broker_url, "/v1/audio/sessions/" + opt.session_id + "/signal/stream");
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
  const std::string auth = std::string("Authorization: Bearer ") + opt.token;
  hdrs = curl_slist_append(hdrs, auth.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, stream_url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, st);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (opt.stream_timeout_ms > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)opt.stream_timeout_ms);
  }

  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (http_status_out) *http_status_out = status;

  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (st->got_offer) return true;
  if (rc != CURLE_OK) {
    st->error = curl_easy_strerror(rc);
  }
  return false;
}

static bool send_signal(const Options& opt, const std::string& type, const Json::Value& payload, std::string* err) {
  Json::Value body(Json::objectValue);
  body["type"] = type;
  if (!payload.isNull()) body["payload"] = payload;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string body_str = Json::writeString(wb, body);

  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/json";
  headers["Authorization"] = std::string("Bearer ") + opt.token;

  const std::string send_url = join_base_path(opt.broker_url, "/v1/audio/sessions/" + opt.session_id + "/signal");
  const HttpClientResult r = http_request(send_url, "POST", headers, body_str, 5000, 256 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (err) {
      if (!r.error.empty()) *err = r.error;
      else *err = "http status " + std::to_string((int)r.http_status);
    }
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) return 2;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    std::cerr << "curl_global_init failed\n";
    return 1;
  }

  SseState st;
  long http_status = 0;
  if (!read_offer_from_stream(opt, &st, &http_status)) {
    std::cerr << "Failed to read offer";
    if (http_status > 0) std::cerr << " (http status " << http_status << ")";
    if (!st.error.empty()) std::cerr << ": " << st.error;
    std::cerr << "\n";
    curl_global_cleanup();
    return 1;
  }

  Json::Value answer_payload(Json::objectValue);
  answer_payload["sdp"] = "stub-answer";
  std::string err;
  if (!send_signal(opt, "answer", answer_payload, &err)) {
    std::cerr << "Failed to send answer: " << err << "\n";
    curl_global_cleanup();
    return 1;
  }

  std::cout << "agentd_audio_signal_loopback OK\n";
  curl_global_cleanup();
  return 0;
}
