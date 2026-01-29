#include "openai_client.h"

#include "agent/agent.h"

#include <curl/curl.h>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <sstream>

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
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

static OpenAIRawResult http_post_json(const OpenAIClientConfig& cfg, const std::string& url, const std::string& body) {
  OpenAIRawResult result;
  result.http_status = 0;

  const char* https_proxy = std::getenv("HTTPS_PROXY");
  if (!https_proxy || !https_proxy[0]) {
    https_proxy = std::getenv("https_proxy");
  }
  const bool have_proxy = (https_proxy && https_proxy[0]);

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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg.timeout_ms);

  if (have_proxy) {
    // Force libcurl to honor the proxy explicitly (and allow us to retry cleanly on failure).
    curl_easy_setopt(curl, CURLOPT_PROXY, https_proxy);
    curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK && have_proxy) {
    // Proxy can be flaky/unavailable. Retry once with proxy disabled so tests can still run.
    response.clear();
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 0L);
    rc = curl_easy_perform(curl);
  }
  if (rc != CURLE_OK) {
    result.response_body = std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
  }

  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  result.http_status = http_status;
  result.response_body = response;

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return result;
}

#if defined(AGENT_HAVE_JSONCPP)
static void append_message_views_json(Json::Value& messages, const agent_message_view_t* views, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const agent_message_view_t& view = views[i];
    Json::Value msg(Json::objectValue);
    msg["role"] = agent_role_to_string(view.role);
    msg["content"] = std::string(view.content, view.content_len);
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
