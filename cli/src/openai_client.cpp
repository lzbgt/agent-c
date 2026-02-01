#include "openai_client.h"

#include "agent/agent.h"

#include "sse_parser.h"

#include <curl/curl.h>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <sstream>
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>

static bool is_retryable_http_status(long http_status) {
  // Common transient statuses across OpenAI-compatible providers.
  // - 408: request timeout
  // - 429: rate limit / quota throttling
  // - 5xx: upstream/provider errors
  return http_status == 408 || http_status == 429 || (http_status >= 500 && http_status <= 599);
}

static bool is_retryable_curl_code(CURLcode rc) {
  // Conservative set of libcurl errors that are generally transient.
  switch (rc) {
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR:
      return true;
    default:
      return false;
  }
}

static int retry_sleep_ms(int attempt) {
  // attempt: 0-based attempt index of the *retry* (not the first try).
  // Backoff: 250, 500, 1000, 2000... up to 4000ms.
  const int base = 250;
  int ms = base << std::max(0, attempt);
  if (ms > 4000) ms = 4000;
  return ms;
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

struct StreamingWriteCtx {
  SseParser parser;
  OpenAIStreamChunkCallback on_chunk = nullptr;
  void* on_chunk_ctx = nullptr;
  std::string capture;
  size_t capture_limit = 0;
  bool saw_done = false;
  size_t chunks = 0;
};

static size_t write_stream_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<StreamingWriteCtx*>(userdata);
  const size_t n = size * nmemb;
  if (!ctx || !ptr || n == 0) return n;

  if (ctx->capture.size() < ctx->capture_limit) {
    const size_t remaining = ctx->capture_limit - ctx->capture.size();
    const size_t to_add = (n <= remaining) ? n : remaining;
    ctx->capture.append(ptr, ptr + to_add);
  }

  std::vector<SseEvent> events;
  ctx->parser.feed(ptr, n, &events);
  for (const auto& ev : events) {
    const std::string data = ev.data;
    if (data == "[DONE]") {
      ctx->saw_done = true;
      continue;
    }
    if (data.empty()) continue;
    ctx->chunks++;
    if (ctx->on_chunk) {
      ctx->on_chunk(ctx->on_chunk_ctx, data.data(), data.size());
    }
  }
  return n;
}

static void ensure_curl_global_init() {
  // libcurl requires global initialization exactly once per process.
  // Without this, multi-threaded usage (daemon async jobs) can exhibit undefined behavior,
  // including apparent "hangs" where no response ever arrives.
  static std::once_flag once;
  std::call_once(once, []() {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
  });
}

static std::string truncate_for_error(const std::string& s, size_t max_len) {
  if (s.size() <= max_len) {
    return s;
  }
  return s.substr(0, max_len) + "...(truncated)";
}

static std::string join_url(const std::string& base, const std::string& path) {
  if (base.empty()) {
    return path;
  }
  if (base.back() == '/' && !path.empty() && path.front() == '/') {
    return base.substr(0, base.size() - 1) + path;
  }
  if (base.back() != '/' && !path.empty() && path.front() != '/') {
    return base + "/" + path;
  }
  return base + path;
}

static std::string normalize_base_url(std::string base_url) {
  // Accept:
  // - https://api.openai.com
  // - https://api.openai.com/v1
  // Normalize to .../v1
  if (base_url.empty()) {
    return base_url;
  }
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  const std::string v1 = "/v1";
  if (base_url.size() >= v1.size() && base_url.compare(base_url.size() - v1.size(), v1.size(), v1) == 0) {
    return base_url;
  }
  return base_url + "/v1";
}

static const char* pick_env_proxy() {
  const char* p = std::getenv("HTTPS_PROXY");
  if (!p || !p[0]) p = std::getenv("https_proxy");
  if (!p || !p[0]) p = std::getenv("HTTP_PROXY");
  if (!p || !p[0]) p = std::getenv("http_proxy");
  return (p && p[0]) ? p : nullptr;
}

static const char* effective_proxy(const OpenAIClientConfig& cfg) {
  if (!cfg.proxy_url.empty()) {
    return cfg.proxy_url.c_str();
  }
  return pick_env_proxy();
}

static OpenAIRawResult http_post_json(const OpenAIClientConfig& cfg, const std::string& url, const std::string& body) {
  OpenAIRawResult result;
  result.http_status = 0;

  ensure_curl_global_init();

  const int max_retries = std::max(0, cfg.max_retries);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    const char* proxy = effective_proxy(cfg);
    const bool have_proxy = (proxy && proxy[0]);

    CURL* curl = curl_easy_init();
    if (!curl) {
      result.response_body = "curl_easy_init failed";
      return result;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!cfg.api_key.empty()) {
      headers = curl_slist_append(headers, ("Authorization: Bearer " + cfg.api_key).c_str());
    }
    if (!cfg.openrouter_http_referer.empty()) {
      headers = curl_slist_append(headers, ("HTTP-Referer: " + cfg.openrouter_http_referer).c_str());
    }
    if (!cfg.openrouter_x_title.empty()) {
      headers = curl_slist_append(headers, ("X-Title: " + cfg.openrouter_x_title).c_str());
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // Make timeouts reliable even when used from background threads.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Total time budget for the request.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg.timeout_ms);
    // Faster failure when the proxy/DNS/route is broken (prevents "hang" perception).
    const long connect_timeout_ms = (cfg.timeout_ms > 0) ? std::min<long>(cfg.timeout_ms, 15000L) : 15000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    // Keep connections from going half-open silently.
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    if (have_proxy) {
      // Force libcurl to honor the proxy explicitly (and allow us to retry cleanly on failure).
      curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
      // Some local HTTP proxies are not happy with HTTP/2 over CONNECT.
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK && have_proxy) {
      // Proxy can be flaky/unavailable. Retry once with proxy disabled so tests can still run.
      response.clear();
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 0L);
      rc = curl_easy_perform(curl);
    }

    long http_status = 0;
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    // Success path.
    if (rc == CURLE_OK && http_status >= 200 && http_status < 300) {
      result.http_status = http_status;
      result.response_body = response;
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    // Decide whether to retry.
    const bool retryable = (rc != CURLE_OK) ? is_retryable_curl_code(rc) : is_retryable_http_status(http_status);
    const bool can_retry = (attempt < max_retries) && retryable;

    // Populate best-effort result for caller if this is the last attempt.
    if (!can_retry) {
      result.http_status = (rc == CURLE_OK) ? http_status : 0;
      result.response_body = (rc == CURLE_OK) ? response : (std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc));
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Backoff before retry.
    const int sleep_ms = retry_sleep_ms(attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }

  // Unreachable, but keep a defensive fallback.
  result.response_body = "http_post_json: unexpected retry loop exit";
  return result;
}

static OpenAIRawResult http_get_raw(const OpenAIClientConfig& cfg, const std::string& url, const std::vector<std::string>& extra_headers) {
  OpenAIRawResult result;
  result.http_status = 0;

  ensure_curl_global_init();

  const int max_retries = std::max(0, cfg.max_retries);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    const char* proxy = effective_proxy(cfg);
    const bool have_proxy = (proxy && proxy[0]);

    CURL* curl = curl_easy_init();
    if (!curl) {
      result.response_body = "curl_easy_init failed";
      return result;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!cfg.api_key.empty()) {
      headers = curl_slist_append(headers, ("Authorization: Bearer " + cfg.api_key).c_str());
    }
    if (!cfg.openrouter_http_referer.empty()) {
      headers = curl_slist_append(headers, ("HTTP-Referer: " + cfg.openrouter_http_referer).c_str());
    }
    if (!cfg.openrouter_x_title.empty()) {
      headers = curl_slist_append(headers, ("X-Title: " + cfg.openrouter_x_title).c_str());
    }
    for (const auto& h : extra_headers) {
      if (!h.empty()) {
        headers = curl_slist_append(headers, h.c_str());
      }
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    // Enable built-in content decoding (gzip/br/etc when supported by libcurl).
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg.timeout_ms);
    const long connect_timeout_ms = (cfg.timeout_ms > 0) ? std::min<long>(cfg.timeout_ms, 15000L) : 15000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    if (have_proxy) {
      curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK && have_proxy) {
      response.clear();
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 0L);
      rc = curl_easy_perform(curl);
    }

    long http_status = 0;
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    if (rc == CURLE_OK && http_status >= 200 && http_status < 300) {
      result.http_status = http_status;
      result.response_body = response;
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    const bool retryable = (rc != CURLE_OK) ? is_retryable_curl_code(rc) : is_retryable_http_status(http_status);
    const bool can_retry = (attempt < max_retries) && retryable;
    if (!can_retry) {
      result.http_status = (rc == CURLE_OK) ? http_status : 0;
      result.response_body = (rc == CURLE_OK) ? response : (std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc));
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    const int sleep_ms = retry_sleep_ms(attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }

  result.response_body = "http_get_raw: unexpected retry loop exit";
  return result;
}

static OpenAIStreamResult http_post_json_stream(
  const OpenAIClientConfig& cfg,
  const std::string& url,
  const std::string& body,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t max_capture_bytes
) {
  OpenAIStreamResult result;
  result.http_status = 0;
  result.saw_done = false;

  ensure_curl_global_init();

  const int max_retries = std::max(0, cfg.max_retries);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    const char* proxy = effective_proxy(cfg);
    const bool have_proxy = (proxy && proxy[0]);

    CURL* curl = curl_easy_init();
    if (!curl) {
      result.response_body = "curl_easy_init failed";
      return result;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    if (!cfg.api_key.empty()) {
      headers = curl_slist_append(headers, ("Authorization: Bearer " + cfg.api_key).c_str());
    }
    if (!cfg.openrouter_http_referer.empty()) {
      headers = curl_slist_append(headers, ("HTTP-Referer: " + cfg.openrouter_http_referer).c_str());
    }
    if (!cfg.openrouter_x_title.empty()) {
      headers = curl_slist_append(headers, ("X-Title: " + cfg.openrouter_x_title).c_str());
    }

    StreamingWriteCtx wctx;
    wctx.on_chunk = on_chunk;
    wctx.on_chunk_ctx = on_chunk_ctx;
    wctx.capture_limit = max_capture_bytes;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg.timeout_ms);
    const long connect_timeout_ms = (cfg.timeout_ms > 0) ? std::min<long>(cfg.timeout_ms, 15000L) : 15000L;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    if (have_proxy) {
      curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
      curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK && have_proxy) {
      // Retry once with proxy disabled.
      wctx.capture.clear();
      wctx.parser.reset();
      wctx.saw_done = false;
      wctx.chunks = 0;
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 0L);
      rc = curl_easy_perform(curl);
    }

    long http_status = 0;
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    }

    // Success (as far as transport is concerned). For streaming, treat non-2xx as error payload.
    if (rc == CURLE_OK && http_status >= 200 && http_status < 300) {
      result.http_status = http_status;
      result.response_body = wctx.capture;
      result.saw_done = wctx.saw_done;
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    // Only retry streaming calls if we didn't emit any chunks (otherwise we'd duplicate partial output).
    const bool can_retry_stream = (wctx.chunks == 0 && !wctx.saw_done);
    const bool retryable =
      can_retry_stream && ((rc != CURLE_OK) ? is_retryable_curl_code(rc) : is_retryable_http_status(http_status));
    const bool can_retry = (attempt < max_retries) && retryable;

    if (!can_retry) {
      result.http_status = (rc == CURLE_OK) ? http_status : 0;
      result.response_body = wctx.capture;
      if (rc != CURLE_OK) {
        result.error_message = std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc);
      }
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return result;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    const int sleep_ms = retry_sleep_ms(attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }

  result.error_message = "http_post_json_stream: unexpected retry loop exit";
  return result;
}

#if defined(AGENT_HAVE_JSONCPP)
static const char* kMultimodalPrefix = "__AGENT_MM_V1__";

static bool try_parse_multimodal_prefix(
  const std::string& content,
  std::string* out_text,
  Json::Value* out_mm
) {
  if (out_text) *out_text = content;
  if (out_mm) *out_mm = Json::Value(Json::nullValue);
  if (!out_text || !out_mm) return false;

  if (content.rfind(kMultimodalPrefix, 0) != 0) return false;
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;
  const size_t prefix_len = std::strlen(kMultimodalPrefix);
  if (nl < prefix_len) return false;

  const std::string json_part = content.substr(prefix_len, nl - prefix_len);
  if (json_part.empty()) return false;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(json_part);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) {
    return false;
  }

  *out_mm = v;
  *out_text = content.substr(nl + 1);
  return true;
}

static Json::Value multimodal_content_from_parts(const std::string& text, const Json::Value& mm) {
  const bool have_images = mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
  const bool have_files = mm.isMember("files") && mm["files"].isArray() && !mm["files"].empty();
  if (!have_images && !have_files) return Json::Value(text);

  Json::Value arr(Json::arrayValue);
  if (!text.empty()) {
    Json::Value t(Json::objectValue);
    t["type"] = "text";
    t["text"] = text;
    arr.append(t);
  }

  if (have_files) {
    for (const auto& f : mm["files"]) {
      if (!f.isObject()) continue;
      const std::string name = f.isMember("name") && f["name"].isString() ? f["name"].asString() : "";
      const std::string mime = f.isMember("mime") && f["mime"].isString() ? f["mime"].asString() : "";
      const std::string ft = f.isMember("text") && f["text"].isString() ? f["text"].asString() : "";
      const bool trunc = f.isMember("truncated") && f["truncated"].isBool() ? f["truncated"].asBool() : false;
      if (ft.empty()) continue;

      std::string block;
      block += "[Attachment";
      if (!name.empty()) block += ": " + name;
      if (!mime.empty()) block += " (" + mime + ")";
      block += "]\n";
      block += ft;
      if (trunc) block += "\n...(truncated)";

      Json::Value t(Json::objectValue);
      t["type"] = "text";
      t["text"] = block;
      arr.append(t);
    }
  }

  if (have_images) {
    for (const auto& im : mm["images"]) {
      if (!im.isObject()) continue;
      const std::string mime = im.isMember("mime") && im["mime"].isString() ? im["mime"].asString() : "image/png";
      const std::string b64 = im.isMember("b64") && im["b64"].isString() ? im["b64"].asString() : "";
      if (b64.empty()) continue;
      const std::string url = std::string("data:") + mime + ";base64," + b64;

      Json::Value part(Json::objectValue);
      part["type"] = "image_url";
      Json::Value iu(Json::objectValue);
      iu["url"] = url;
      part["image_url"] = iu;
      arr.append(part);
    }
  }
  return arr;
}

static void append_message_views_json(Json::Value& messages, const agent_message_view_t* views, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const agent_message_view_t& view = views[i];
    Json::Value msg(Json::objectValue);
    msg["role"] = agent_role_to_string(view.role);
    const std::string raw(view.content, view.content_len);
    std::string text = raw;
    Json::Value mm(Json::nullValue);
    if (try_parse_multimodal_prefix(raw, &text, &mm) && mm.isObject()) {
      msg["content"] = multimodal_content_from_parts(text, mm);
    } else {
      msg["content"] = raw;
    }
    messages.append(msg);
  }
}

static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static std::string json_try_extract_assistant_text(const Json::Value& root) {
  // OpenAI-compatible: choices[0].message.content
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) {
    return "";
  }
  const auto& message = choices[0]["message"];
  const auto& content = message["content"];
  if (content.isString()) {
    return content.asString();
  }
  // Some providers may return "text" at choices[0].text (older formats).
  const auto& text = choices[0]["text"];
  if (text.isString()) {
    return text.asString();
  }
  return "";
}

static std::string json_try_extract_error_message(const Json::Value& root) {
  // OpenAI: {"error":{"message":"...", ...}}
  const auto& e = root["error"];
  if (e.isObject()) {
    const auto& msg = e["message"];
    if (msg.isString()) {
      return msg.asString();
    }
    const auto& s = e["error"];
    if (s.isString()) {
      return s.asString();
    }
  } else if (e.isString()) {
    return e.asString();
  }

  // Some providers may use {"message":"..."} or {"detail":"..."}.
  const auto& msg = root["message"];
  if (msg.isString()) {
    return msg.asString();
  }
  const auto& detail = root["detail"];
  if (detail.isString()) {
    return detail.asString();
  }
  return "";
}
#endif

OpenAIChatResult openai_chat_completions(
  const OpenAIClientConfig& cfg,
  const agent_message_view_t* messages_view,
  size_t message_count
) {
  OpenAIChatResult result;
  result.http_status = 0;

  const std::string base = normalize_base_url(cfg.base_url);
  const std::string url = join_url(base, "/chat/completions");

#if defined(AGENT_HAVE_JSONCPP)
  Json::Value root(Json::objectValue);
  root["model"] = cfg.model;
  root["stream"] = false;
  Json::Value messages(Json::arrayValue);
  append_message_views_json(messages, messages_view, message_count);
  root["messages"] = messages;
  const std::string body = json_stringify(root);
#else
  (void)messages_view;
  (void)message_count;
  const std::string body = "{}";
#endif

  OpenAIRawResult raw = http_post_json(cfg, url, body);
  result.http_status = raw.http_status;
  result.response_body = raw.response_body;

#if defined(AGENT_HAVE_JSONCPP)
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(result.response_body);
  Json::Value parsed;
  if (Json::parseFromStream(rb, iss, &parsed, &errs)) {
    result.assistant_text = json_try_extract_assistant_text(parsed);
    result.error_message = json_try_extract_error_message(parsed);
  }
#endif
  return result;
}

OpenAIRawResult openai_chat_completions_raw(const OpenAIClientConfig& cfg, const std::string& request_body_json) {
  const std::string base = normalize_base_url(cfg.base_url);
  const std::string url = join_url(base, "/chat/completions");
  return http_post_json(cfg, url, request_body_json);
}

OpenAIRawResult openai_http_get_raw(const OpenAIClientConfig& cfg, const std::string& url, const std::vector<std::string>& extra_headers) {
  return http_get_raw(cfg, url, extra_headers);
}

OpenAIStreamResult openai_chat_completions_raw_stream(
  const OpenAIClientConfig& cfg,
  const std::string& request_body_json,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t max_capture_bytes
) {
  const std::string base = normalize_base_url(cfg.base_url);
  const std::string url = join_url(base, "/chat/completions");
  OpenAIStreamResult r = http_post_json_stream(cfg, url, request_body_json, on_chunk, on_chunk_ctx, max_capture_bytes);

#if defined(AGENT_HAVE_JSONCPP)
  // Best-effort: if the provider returned a non-stream JSON error body, extract an error message.
  if (r.http_status >= 400 && !r.response_body.empty()) {
    r.error_message = openai_try_extract_error_message(r.response_body);
  }
#endif
  return r;
}

std::string openai_try_extract_error_message(const std::string& response_body) {
#if !defined(AGENT_HAVE_JSONCPP)
  (void)response_body;
  return "";
#else
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(response_body);
  Json::Value root;
  if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject()) {
    return "";
  }
  return json_try_extract_error_message(root);
#endif
}

std::string openai_format_http_error(long http_status, const std::string& response_body) {
  std::string msg = "HTTP " + std::to_string(http_status);
  const std::string extracted = openai_try_extract_error_message(response_body);
  if (!extracted.empty()) {
    msg += ": " + extracted;
    return msg;
  }
  if (!response_body.empty()) {
    msg += ": " + truncate_for_error(response_body, 240);
  }
  return msg;
}

bool openai_is_context_too_long_error(long http_status, const std::string& response_body) {
  // Some providers return HTTP 413 when the prompt is too large.
  if (http_status == 413) {
    return true;
  }
  if (http_status < 400) {
    return false;
  }

  std::string msg = openai_try_extract_error_message(response_body);
  if (msg.empty()) {
    msg = response_body;
  }

  // Lowercase scan for common "context too long" / "too many tokens" patterns across providers.
  std::string s;
  s.reserve(msg.size());
  for (char c : msg) {
    s.push_back((char)std::tolower((unsigned char)c));
  }

  const char* needles[] = {
    "context length",
    "maximum context",
    "max context",
    "context window",
    "too many tokens",
    "token limit",
    "prompt is too long",
    "request too large",
    "reduce the length",
    "exceeds the maximum",
    "context_length_exceeded",
  };
  for (const char* n : needles) {
    if (s.find(n) != std::string::npos) {
      return true;
    }
  }
  return false;
}
