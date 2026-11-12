#include "toolset_host_web.h"
#include "toolset_host_internal.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace host_tools_internal {
namespace {

constexpr size_t kMaxArgumentsBytes = 8192;
constexpr size_t kMaxQueryBytes = 1024;
constexpr size_t kMaxResponseBytes = 256 * 1024;
constexpr int64_t kTimeoutMs = 15000;
constexpr const char* kEndpoint = "https://api.mwmbl.org/api/v2/search/?q=";

agent_status_t finish(agent_string_t* out, const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return set_result(out, Json::writeString(writer, value));
}

agent_status_t fail(agent_string_t* out, const std::string& error, long status = 0) {
  Json::Value value(Json::objectValue);
  value["ok"] = false;
  value["error"] = error;
  value["data"]["source"] = "Mwmbl";
  if (status > 0) value["data"]["http_status"] = Json::Int64(status);
  return finish(out, value);
}

bool parse_json(const char* text, size_t length, Json::Value& value) {
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  builder["stackLimit"] = 32;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string error;
  try { return reader->parse(text, text + length, &value, &error); }
  catch (const Json::Exception&) { return false; }
}

bool valid_utf8(const std::string& text) {
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) { if (!first) return false; continue; }
    size_t count;
    unsigned cp, minimum;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; cp = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; cp = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; cp = first & 0x07; minimum = 0x10000; }
    else return false;
    if (count > text.size() - i) return false;
    while (count--) {
      const auto c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}

bool shorten_utf8(std::string& text, size_t limit) {
  if (text.size() <= limit) return false;
  size_t end = limit;
  while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  text.resize(end);
  return true;
}

void trim(std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) { text.clear(); return; }
  text.erase(text.find_last_not_of(" \t\r\n") + 1);
  text.erase(0, first);
}

bool valid_url(const std::string& url) {
  if (url.empty() || url.size() > 4096 || !valid_utf8(url)) return false;
  for (size_t i = 0; i < url.size(); ++i) {
    const auto c = static_cast<unsigned char>(url[i]);
    if (c <= 0x20 || c == 0x7f || c == '\\') return false;
    if (c == 0xc2 && i + 1 < url.size()) {
      const auto next = static_cast<unsigned char>(url[i + 1]);
      if (next >= 0x80 && next <= 0x9f) return false;
    }
  }
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return false;
  auto part = [&](CURLUPart which, std::string* value = nullptr) {
    char* raw = nullptr;
    const bool present = curl_url_get(parsed.get(), which, &raw, 0) == CURLUE_OK;
    if (present && value) *value = raw;
    curl_free(raw);
    return present;
  };
  std::string scheme, host;
  if (!part(CURLUPART_SCHEME, &scheme) || !part(CURLUPART_HOST, &host) || host.empty()) return false;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return (scheme == "http" || scheme == "https") && !part(CURLUPART_USER) && !part(CURLUPART_PASSWORD);
}

std::string encode_query(const std::string& query) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(query.size() * 3);
  for (unsigned char c : query) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

}  // namespace

agent_status_t web_search_execute(
    const char* arguments_json,
    agent_string_t* out_result,
    WebSearchHttpRequest request) {
  if (!arguments_json || !out_result || !request) return AGENT_ERR_INVALID_ARGUMENT;
  size_t length = 0;
  while (length <= kMaxArgumentsBytes && arguments_json[length]) ++length;
  if (length > kMaxArgumentsBytes) return fail(out_result, "web_search arguments exceed 8192 bytes");

  Json::Value args;
  if (!parse_json(arguments_json, length, args) || !args.isObject()) {
    return fail(out_result, "web_search arguments must be a JSON object");
  }
  for (const auto& key : args.getMemberNames()) {
    if (key != "query" && key != "max_results") return fail(out_result, "Unknown web_search argument");
  }
  if (!args["query"].isString()) return fail(out_result, "query must be a nonempty string");
  const std::string query = args["query"].asString();
  if (query.empty() || query.size() > kMaxQueryBytes || !valid_utf8(query)) {
    return fail(out_result, "query must contain 1 to 1024 valid UTF-8 bytes without NUL");
  }
  bool nonblank = false;
  for (unsigned char c : query) nonblank = nonblank || !std::isspace(c);
  if (!nonblank) return fail(out_result, "query must not be blank");

  size_t max_results = 5;
  if (args.isMember("max_results")) {
    if (!args["max_results"].isInt() || args["max_results"].asInt() < 1 || args["max_results"].asInt() > 10) {
      return fail(out_result, "max_results must be an integer from 1 to 10");
    }
    max_results = static_cast<size_t>(args["max_results"].asInt());
  }

  const std::string url = kEndpoint + encode_query(query);
  // No provider credentials, cookies, user-selected destinations or redirects.
  // Empty proxy selects the existing native client's HTTP[S]_PROXY/NO_PROXY handling.
  const auto response = request(url, "GET", {{"Accept", "application/json"}, {"User-Agent", "AgentC/1.0"}},
      "", kTimeoutMs, kMaxResponseBytes, "", nullptr);
  if (response.timed_out) return fail(out_result, "Search request timed out", response.http_status);
  if (response.response_too_large || response.response_body.size() > kMaxResponseBytes) {
    return fail(out_result, "Search response exceeded 262144 bytes", response.http_status);
  }
  if (!response.ok) return fail(out_result, "Search transport failed", response.http_status);
  if (response.http_status != 200) return fail(out_result, "Search endpoint returned a non-200 response", response.http_status);

  Json::Value feed;
  if (!valid_utf8(response.response_body) || !parse_json(response.response_body.data(), response.response_body.size(), feed) || !feed.isObject() ||
      !feed["query"].isString() || feed["query"].asString() != query || !feed["results"].isArray()) {
    return fail(out_result, "Invalid search JSON or mismatched query", response.http_status);
  }
  Json::Value value(Json::objectValue);
  value["ok"] = true;
  auto& data = value["data"];
  data["query"] = query;
  data["source"] = "Mwmbl";
  data["source_url"] = url;
  data["results"] = Json::Value(Json::arrayValue);
  bool truncated = false;
  size_t skipped = 0;
  const auto& results = feed["results"];
  for (Json::ArrayIndex i = 0; i < results.size(); ++i) {
    if (data["results"].size() >= max_results || i >= 100) { truncated = true; break; }
    const auto& item = results[i];
    if (!item.isObject() || !item["title"].isString() || !item["url"].isString() ||
        (item.isMember("content") && !item["content"].isString())) { ++skipped; continue; }
    auto title = item["title"].asString();
    auto link = item["url"].asString();
    auto snippet = item.get("content", "").asString();
    trim(title); trim(link); trim(snippet);
    if (title.empty() || !valid_utf8(title) || !valid_utf8(snippet) || !valid_url(link)) { ++skipped; continue; }
    const bool title_cut = shorten_utf8(title, 512);
    const bool snippet_cut = shorten_utf8(snippet, 1000);
    truncated = truncated || title_cut || snippet_cut;
    Json::Value entry(Json::objectValue);
    entry["title"] = title;
    entry["url"] = link;
    entry["snippet"] = snippet;
    data["results"].append(entry);
  }
  data["truncated"] = truncated;
  data["skipped_items"] = Json::UInt64(skipped);
  return finish(out_result, value);
}

agent_status_t tool_web_search(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) return fail(out_result, "Search cancelled");
  return web_search_execute(arguments_json, out_result, agentd::http_request);
}

}  // namespace host_tools_internal
