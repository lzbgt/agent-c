#include "web_discovery.h"
#include "browser_process.h"

#include <curl/curl.h>
#include <json/json.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

namespace host_tools_internal {
namespace {
constexpr size_t kResponseLimit = 1024 * 1024;
constexpr size_t kCandidateLimit = 128;
constexpr size_t kCacheLimit = 64;
constexpr int64_t kCacheTtlMs = 300000;
constexpr int64_t kPartialTtlMs = 30000;

struct CacheEntry {
  WebDiscoveryResult result;
  int64_t fetched_ms = 0;
  int64_t used_ms = 0;
};
std::mutex cache_mutex;
std::map<std::string, CacheEntry> cache;

int64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string utc_now() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  if (!gmtime_r(&now, &utc)) return "";
  char value[32];
  return std::strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &utc) ? value : "";
}
bool valid_utf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) continue;
    unsigned value = 0, count = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) { value = first & 31; count = 1; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { value = first & 15; count = 2; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; count = 3; minimum = 0x10000; }
    else return false;
    if (text.size() - i < count) return false;
    while (count--) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
std::string clip_utf8(std::string text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t end = limit;
  while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  text.resize(end);
  return text;
}
std::string lower(std::string text) {
  for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}
void collect_text(xmlNode* node, std::string& out, size_t& nodes, int depth) {
  if (depth > 64) return;
  for (; node && nodes < 2048 && out.size() < 32768; node = node->next) {
    ++nodes;
    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
      if (node->content) {
        const auto* data = reinterpret_cast<const char*>(node->content);
        out.append(data, std::min<size_t>(xmlStrlen(node->content), 32768 - out.size()));
      }
      continue;
    }
    if (node->type != XML_ELEMENT_NODE) continue;
    const auto name = lower(reinterpret_cast<const char*>(node->name));
    if (name == "script" || name == "style" || name == "template" ||
        name == "noscript" || name == "iframe" || name == "object") continue;
    collect_text(node->children, out, nodes, depth + 1);
    if (out.size() < 32768) out.push_back(' ');
  }
}
std::string clean_text(const std::string& raw, size_t limit) {
  if (!valid_utf8(raw)) return "";
  auto text = clip_utf8(raw, 16384);
  if (text.find_first_of("<&") != std::string::npos) {
    const auto html = "<html><body>" + text + "</body></html>";
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, "UTF-8",
                       HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING),
        xmlFreeDoc);
    if (!doc) return "";
    text.clear();
    size_t nodes = 0;
    collect_text(xmlDocGetRootElement(doc.get()), text, nodes, 0);
  }
  std::string out;
  bool space = false;
  for (unsigned char c : text) {
    if (c <= 0x20 || c == 0x7f) { space = !out.empty(); continue; }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(static_cast<char>(c));
  }
  return valid_utf8(out) ? clip_utf8(std::move(out), limit) : "";
}
std::string url_part(CURLU* parsed, CURLUPart part) {
  char* value = nullptr;
  if (curl_url_get(parsed, part, &value, 0) != CURLUE_OK) return "";
  std::string out(value);
  curl_free(value);
  return out;
}
bool has_url_part(CURLU* parsed, CURLUPart part) {
  char* value = nullptr;
  const bool present = curl_url_get(parsed, part, &value, 0) == CURLUE_OK;
  curl_free(value);
  return present;
}
bool clean_url_bytes(const std::string& url) {
  if (url.empty() || url.size() > 4096 || !valid_utf8(url)) return false;
  for (unsigned char c : url) if (c <= 0x20 || c == 0x7f || c == '\\') return false;
  return true;
}
std::string endpoint(const std::string& raw) {
  if (!clean_url_bytes(raw)) return "";
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, raw.c_str(), 0) != CURLUE_OK) return "";
  const auto scheme = url_part(parsed.get(), CURLUPART_SCHEME);
  const auto path = url_part(parsed.get(), CURLUPART_PATH);
  const auto port = url_part(parsed.get(), CURLUPART_PORT);
  if ((scheme != "http" && scheme != "https") || (path != "" && path != "/") || port.empty() ||
      has_url_part(parsed.get(), CURLUPART_USER) || has_url_part(parsed.get(), CURLUPART_PASSWORD) ||
      has_url_part(parsed.get(), CURLUPART_QUERY) || has_url_part(parsed.get(), CURLUPART_FRAGMENT)) return "";
  auto host = url_part(parsed.get(), CURLUPART_HOST);
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  in_addr v4{};
  in6_addr v6{};
  const bool loopback = (inet_pton(AF_INET, host.c_str(), &v4) == 1 && (ntohl(v4.s_addr) >> 24) == 127) ||
                        (inet_pton(AF_INET6, host.c_str(), &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6));
  if (!loopback || curl_url_set(parsed.get(), CURLUPART_PATH, "/search", 0) != CURLUE_OK) return "";
  return url_part(parsed.get(), CURLUPART_URL);
}
std::string public_url(const std::string& raw) {
  if (!clean_url_bytes(raw) || !browser_http_url(raw)) return "";
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, raw.c_str(), 0) != CURLUE_OK) return "";
  const auto host = lower(url_part(parsed.get(), CURLUPART_HOST));
  if (curl_url_set(parsed.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK ||
      curl_url_set(parsed.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK) return "";
  char* value = nullptr;
  if (curl_url_get(parsed.get(), CURLUPART_URL, &value, CURLU_NO_DEFAULT_PORT) != CURLUE_OK) return "";
  std::string out(value);
  curl_free(value);
  return out.size() <= 4096 ? out : "";
}
void add_engine(std::vector<std::string>& engines, const std::string& raw) {
  const auto engine = clean_text(raw, 128);
  if (!engine.empty() && engines.size() < 8 &&
      std::find(engines.begin(), engines.end(), engine) == engines.end()) engines.push_back(engine);
}
bool parse_json(const std::string& body, Json::Value& root) {
  if (body.empty() || body.size() > kResponseLimit || !valid_utf8(body)) return false;
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  builder["stackLimit"] = 64;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string error;
  try { return reader->parse(body.data(), body.data() + body.size(), &root, &error) && root.isObject(); }
  catch (const Json::Exception&) { return false; }
}
bool denied(const std::string& error) {
  const auto text = lower(error);
  return text.find("captcha") != std::string::npos || text.find("access denied") != std::string::npos ||
         text.find("too many requests") != std::string::npos || text.find("forbidden") != std::string::npos;
}
void parse_results(const Json::Value& root, WebDiscoveryResult& out, size_t max_results) {
  if (root.isMember("error")) {
    out.error = "Local discovery service returned an error";
    if (root["error"].isString()) {
      const auto detail = clean_text(root["error"].asString(), 256);
      if (!detail.empty()) out.error += ": " + detail;
    }
    return;
  }
  if (!root["results"].isArray() ||
      (root.isMember("unresponsive_engines") && !root["unresponsive_engines"].isArray())) {
    out.error = "Invalid discovery result schema";
    return;
  }
  const auto& failures = root["unresponsive_engines"];
  for (Json::ArrayIndex i = 0; i < failures.size() && i < 32; ++i) {
    const auto& item = failures[i];
    if (!item.isArray() || item.size() != 2 || !item[0].isString() || !item[1].isString()) {
      out.error = "Invalid discovery engine failure metadata";
      return;
    }
    out.engine_failures.push_back({clean_text(item[0].asString(), 128), clean_text(item[1].asString(), 512)});
  }
  const auto& results = root["results"];
  std::map<std::string, size_t> seen;
  // SearXNG returns ordered results. Retain that ranking instead of inventing a
  // local relevance score; duplicates inherit the first ranked occurrence.
  for (Json::ArrayIndex i = 0; i < results.size() && i < kCandidateLimit; ++i) {
    const auto& item = results[i];
    if (!item.isObject() || !item["title"].isString() || !item["url"].isString()) {
      ++out.skipped_results; continue;
    }
    WebDiscoveryEntry entry;
    entry.title = clean_text(item["title"].asString(), 512);
    entry.url = public_url(item["url"].asString());
    if (entry.title.empty() || entry.url.empty()) { ++out.skipped_results; continue; }
    if (item["content"].isString()) entry.snippet = clean_text(item["content"].asString(), 1000);
    if (item["engine"].isString()) add_engine(entry.engines, item["engine"].asString());
    if (item["engines"].isArray()) {
      for (Json::ArrayIndex n = 0; n < item["engines"].size() && n < 16; ++n)
        if (item["engines"][n].isString()) add_engine(entry.engines, item["engines"][n].asString());
    }
    if (item["score"].isNumeric()) {
      const double score = item["score"].asDouble();
      if (std::isfinite(score)) entry.score = score;
    }
    const auto previous = seen.find(entry.url);
    if (previous != seen.end()) {
      auto& existing = out.results[previous->second];
      for (const auto& engine : entry.engines) add_engine(existing.engines, engine);
      existing.score = std::max(existing.score, entry.score);
      continue;
    }
    if (out.results.size() >= max_results) continue;
    seen.emplace(entry.url, out.results.size());
    out.results.push_back(std::move(entry));
  }
  out.partial = !out.engine_failures.empty() || out.skipped_results > 0 ||
                results.size() > kCandidateLimit || failures.size() > 32;
  if (out.results.empty()) {
    if (!results.empty()) out.error = "Discovery returned no valid public results";
    else if (!out.engine_failures.empty()) {
      out.error = "Discovery returned no results and upstream engines failed";
      for (const auto& failure : out.engine_failures) out.blocked = out.blocked || denied(failure.error);
    } else out.empty = true;
  }
  if (out.partial) {
    out.warning = "Search coverage is partial; inspect engine_failures and skipped_results.";
    out.quality = "partial";
  }
  out.ok = out.error.empty();
}
}  // namespace

WebDiscoveryResult web_discover_with_transport(
    const WebDiscoveryRequest& request, WebDiscoveryHttpRequest transport,
    HostCancelCallback cancel, void* context, WebDiscoveryClock clock) {
  WebDiscoveryResult out;
  const char* configured = std::getenv("AGENT_SEARXNG_URL");
  if (!configured || !*configured) return out;
  out.configured = true;
  out.backend = "searxng";
  const auto base = endpoint(configured);
  if (base.empty()) {
    out.error = "AGENT_SEARXNG_URL must be an HTTP(S) origin with a literal loopback IP and explicit port";
    return out;
  }
  if (!transport || request.query.empty() || request.query.size() > 1024 ||
      !valid_utf8(request.query) || request.max_results < 1 || request.max_results > 10 ||
      request.timeout_ms < 1000 || request.timeout_ms > 30000) {
    out.error = "Invalid discovery request";
    return out;
  }
  bool meaningful = false;
  for (unsigned char c : request.query) {
    if (c < 0x20 || c == 0x7f) { out.error = "Invalid discovery query"; return out; }
    meaningful = meaningful || c != ' ';
  }
  if (!meaningful) { out.error = "Discovery query is empty"; return out; }
  auto stopped = [&]() {
    if (cancel && cancel(context)) { out.cancelled = true; out.error = "Discovery cancelled"; return true; }
    return false;
  };
  if (stopped()) return out;
  const auto now = clock ? clock : monotonic_ms;
  const auto started = now();
  const std::string cache_key = base + "\n" + request.query + "\n" + std::to_string(request.max_results);
  {
    std::lock_guard<std::mutex> guard(cache_mutex);
    auto found = cache.find(cache_key);
    if (request.fresh) {
      if (found != cache.end()) cache.erase(found);
    } else if (found != cache.end()) {
      const auto age = started - found->second.fetched_ms;
      const auto ttl = found->second.result.partial ? kPartialTtlMs : kCacheTtlMs;
      if (age >= 0 && age < ttl) {
        found->second.used_ms = started;
        out = found->second.result;
        out.cache_hit = true;
        out.cache_age_ms = age;
        return out;
      }
      cache.erase(found);
    }
  }
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) { out.error = "Cannot encode discovery query"; return out; }
  char* encoded = curl_easy_escape(curl.get(), request.query.data(), static_cast<int>(request.query.size()));
  if (!encoded) { out.error = "Cannot encode discovery query"; return out; }
  const std::string url = base + "?format=json&q=" + encoded;
  curl_free(encoded);
  const std::map<std::string, std::string> headers = {
      {"Accept", "application/json"}, {"Accept-Encoding", "gzip, deflate"}, {"User-Agent", "AgentC/1.0"}};
  // Endpoint is operator-owned loopback. Never forward provider headers,
  // credentials or environment proxies; redirects stay disabled in http_request.
  const auto response = transport(url, "GET", headers, "", request.timeout_ms, kResponseLimit,
                                  "", nullptr, cancel, context, false);
  out.http_status = response.http_status;
  out.blocked = response.http_status == 401 || response.http_status == 403 || response.http_status == 429;
  out.cancelled = response.cancelled;
  out.timed_out = response.timed_out;
  if (out.http_status > 0) out.fetched_at = utc_now();
  if (out.cancelled) { out.error = "Discovery cancelled"; return out; }
  if (stopped()) return out;
  if (!response.ok) {
    out.error = response.timed_out ? "Discovery timed out" :
        response.response_too_large ? "Discovery response exceeds byte limit" :
        response.http_status > 0 && response.http_status != 200
            ? "Local discovery returned HTTP " + std::to_string(response.http_status)
            : "Local discovery request failed";
    return out;
  }
  if (response.http_status != 200) {
    out.blocked = response.http_status == 401 || response.http_status == 403 || response.http_status == 429;
    out.error = "Local discovery returned HTTP " + std::to_string(response.http_status);
    return out;
  }
  const auto type = response.response_headers.find("content-type");
  const auto mime = type == response.response_headers.end() ? "" : lower(type->second.substr(0, type->second.find(';')));
  if (mime != "application/json") {
    out.error = "Local discovery did not return JSON";
    return out;
  }
  Json::Value root;
  if (!parse_json(response.response_body, root)) { out.error = "Invalid or oversized discovery JSON"; return out; }
  try { parse_results(root, out, request.max_results); }
  catch (const Json::Exception&) {
    out.results.clear(); out.empty = false; out.ok = false; out.error = "Invalid discovery result values";
  }
  if (stopped()) { out.ok = false; out.results.clear(); return out; }
  if (out.ok) {
    const auto fetched = now();
    std::lock_guard<std::mutex> guard(cache_mutex);
    if (cache.size() >= kCacheLimit && cache.find(cache_key) == cache.end()) {
      const auto oldest = std::min_element(cache.begin(), cache.end(),
          [](const auto& a, const auto& b) { return a.second.used_ms < b.second.used_ms; });
      if (oldest != cache.end()) cache.erase(oldest);
    }
    cache[cache_key] = {out, fetched, fetched};
  }
  return out;
}

WebDiscoveryResult web_discover(
    const WebDiscoveryRequest& request, HostCancelCallback cancel, void* context) {
  return web_discover_with_transport(request, agentd::http_request, cancel, context);
}

}  // namespace host_tools_internal
