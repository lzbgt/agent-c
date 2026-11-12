#include "web_fetch.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace host_tools_internal {
namespace {
constexpr size_t kBodyCap = 2 * 1024 * 1024;
constexpr size_t kTextCap = 128000;
constexpr int64_t kCacheTtl = 5 * 60 * 1000;
int64_t monotonic_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string timestamp() {
  const auto now = std::time(nullptr); std::tm time{}; gmtime_r(&now, &time);
  char out[32]; std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &time); return out;
}
std::string lower(std::string text) { for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return text; }
std::string trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
void remove_partial_utf8(std::string& value) {
  if (value.empty()) return;
  size_t start = value.size() - 1;
  while (start && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80) --start;
  const auto first = static_cast<unsigned char>(value[start]);
  const size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
  if (value.size() - start < width) value.resize(start);
}
std::string normalize(const std::string& value, size_t limit) {
  std::string out; out.reserve(std::min(limit, value.size())); bool space = false;
  for (unsigned char c : value) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { space = !out.empty(); continue; }
    if (!c) continue;
    if (space && out.size() < limit) out.push_back(' ');
    space = false; if (out.size() >= limit) break; out.push_back(static_cast<char>(c));
  }
  remove_partial_utf8(out);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
std::string node_text(xmlNode* node, size_t limit) {
  xmlChar* value = xmlNodeGetContent(node); if (!value) return "";
  const auto text = normalize(reinterpret_cast<const char*>(value), limit); xmlFree(value); return text;
}
std::string property(xmlNode* node, const char* name) {
  xmlChar* value = xmlGetProp(node, BAD_CAST name); if (!value) return "";
  std::string out(reinterpret_cast<const char*>(value)); xmlFree(value); return out;
}
bool named(xmlNode* node, const char* name) { return node && node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name); }
xmlNode* find(xmlNode* node, const char* name) {
  for (; node; node = node->next) {
    if (named(node, name)) return node;
    if (auto* child = find(node->children, name)) return child;
  }
  return nullptr;
}
bool challenge_element(xmlNode* node) {
  for (; node; node = node->next) {
    if (node->type == XML_ELEMENT_NODE) {
      const auto id = lower(property(node, "id"));
      if (id == "challenge-form" || id == "cf-challenge-running" || id == "px-captcha") return true;
    }
    if (challenge_element(node->children)) return true;
  }
  return false;
}
bool excluded(xmlNode* node) {
  if (node->type != XML_ELEMENT_NODE) return false;
  const std::string name(reinterpret_cast<const char*>(node->name));
  // Some publishers wrap their primary article and data tables in an aside.
  // Keep it traversable; main/article selection and hidden checks choose content.
  static const std::set<std::string> tags = {"script","style","nav","header","footer","form","noscript","iframe","object","svg","canvas","template"};
  if (tags.count(name) || xmlHasProp(node, BAD_CAST "hidden") || lower(property(node, "aria-hidden")) == "true") return true;
  const auto style = lower(property(node, "style"));
  return style.find("display:none") != std::string::npos || style.find("display: none") != std::string::npos || style.find("visibility:hidden") != std::string::npos;
}
xmlNode* find_visible(xmlNode* node, const char* name) {
  for (; node; node = node->next) {
    if (excluded(node)) continue;
    if (named(node, name)) return node;
    if (auto* child = find_visible(node->children, name)) return child;
  }
  return nullptr;
}
std::string resolve(const std::string& base, const std::string& relative) {
  if (relative.empty() || relative.size() > 4096) return "";
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, base.c_str(), 0) != CURLUE_OK || curl_url_set(url.get(), CURLUPART_URL, relative.c_str(), 0) != CURLUE_OK) return "";
  char* raw = nullptr; if (curl_url_get(url.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) return "";
  std::string out(raw); curl_free(raw); return out;
}
struct Extraction {
  Json::Value page;
  bool blocked = false, shell = false, cancelled = false;
  std::string error;
};
Extraction extract_html(const std::string& body, const std::string& url, const std::string& charset, bool allow_local, HostCancelCallback cancel, void* context) {
  Extraction out;
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(htmlReadMemory(body.data(), static_cast<int>(body.size()), url.c_str(), charset.empty() ? nullptr : charset.c_str(),
      HTML_PARSE_NONET | HTML_PARSE_NODEFDTD | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT), xmlFreeDoc);
  if (!doc) { out.error = "Cannot parse HTML response"; return out; }
  auto* root = xmlDocGetRootElement(doc.get());
  auto* body_node = find(root, "body"); if (!body_node) body_node = root;
  auto* title_node = find(root, "title");
  const auto title = title_node ? node_text(title_node, 512) : "";
  const auto title_lower = lower(title);
  const auto beginning = lower(node_text(body_node, 8000));
  const auto raw_lower = lower(body.substr(0, std::min<size_t>(body.size(), 262144)));
  const auto is_gate = [&] {
    if ((title_lower == "oh noes!" && beginning.find("access denied") != std::string::npos && beginning.find("anubis") != std::string::npos) ||
        (title_lower.find("making sure") == 0 && title_lower.find("not a bot") != std::string::npos && raw_lower.find("anubis") != std::string::npos)) return true;
    if (title_lower.find("just a moment") == 0 || title_lower == "access denied" || title_lower == "403 forbidden" || title_lower == "robot check" || title_lower == "human verification") return true;
    if (title == "百度安全验证" || title == "百度访问验证" || url.find("www.google.com/sorry/") != std::string::npos) return true;
    if (challenge_element(root)) return true;
    if (raw_lower.find("/anomaly.js") != std::string::npos && beginning.find("bots use duckduckgo too") != std::string::npos) return true;
    return (title_lower.find("sign in to continue") == 0 || title_lower.find("login to continue") == 0) && raw_lower.find("password") != std::string::npos;
  };
  out.page["url"] = url; out.page["title"] = title; out.page["content_type"] = "text/html";
  if (is_gate()) { out.blocked = true; out.error = "HTTP page contains an access challenge or sign-in gate"; return out; }
  auto* main = find_visible(body_node, "main"); if (!main) main = find_visible(body_node, "article"); if (!main) main = body_node;
  std::string text; bool truncated = false; size_t nodes = 0;
  std::set<std::string> seen;
  Json::Value links(Json::arrayValue), metadata(Json::objectValue);
  std::string base = url;
  if (auto* base_node = find(root, "base")) { auto candidate = resolve(url, property(base_node, "href")); if (browser_http_url(candidate, allow_local)) base = candidate; }
  const auto walk = [&](auto&& self, xmlNode* node, bool siblings) -> void {
    for (; node; node = siblings ? node->next : nullptr) {
      if ((++nodes & 63) == 0 && cancel && cancel(context)) { out.cancelled = true; return; }
      if (out.cancelled) return;
      if (excluded(node)) continue;
      if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
        const auto* raw = reinterpret_cast<const char*>(node->content);
        if (raw && text.size() < kTextCap) {
          const size_t length = std::strlen(raw), amount = std::min(length, kTextCap - text.size());
          text.append(raw, amount); truncated = truncated || amount < length;
          if (text.size() < kTextCap) text.push_back(' ');
        } else if (raw && *raw) truncated = true;
      }
      if (named(node, "a") && links.size() < 20) {
        const auto destination = resolve(base, property(node, "href")); const auto label = node_text(node, 512);
        if (!label.empty() && browser_http_url(destination, allow_local) && seen.insert(destination).second) { Json::Value link; link["title"] = label; link["url"] = destination; links.append(link); }
      }
      self(self, node->children, true);
    }
  };
  walk(walk, main, false);
  if (out.cancelled) { out.error = "HTTP page extraction cancelled"; return out; }
  const auto meta_walk = [&](auto&& self, xmlNode* node) -> void {
    for (; node; node = node->next) {
      if (named(node, "meta")) {
        auto key = lower(property(node, "property")); if (key.empty()) key = lower(property(node, "name"));
        if (key == "article:published_time" || key == "article:modified_time" || key == "datepublished" || key == "datemodified") {
          const auto value = property(node, "content"); if (!value.empty() && value.size() <= 128) metadata[key] = value;
        }
      }
      self(self, node->children);
    }
  };
  meta_walk(meta_walk, root);
  remove_partial_utf8(text);
  text = normalize(text, kTextCap);
  out.page["text"] = text; out.page["text_truncated"] = truncated;
  out.page["links"] = links; out.page["metadata"] = metadata;
  out.shell = text.size() < 160 && (find(root, "script") || beginning.find("enable javascript") != std::string::npos);
  return out;
}
struct CacheEntry { BrowserResult result; int64_t at = 0; };
std::mutex cache_mutex;
std::map<std::string, CacheEntry> cache;
BrowserResult error(const std::string& message, bool blocked = false) {
  BrowserResult out; out.source = "http"; out.rendered = false; out.error = message; out.blocked = blocked; return out;
}
}  // namespace

BrowserResult web_fetch_with_transport(const BrowserRequest& request, WebFetchHttpRequest http, BrowserReadPage browser,
    HostCancelCallback cancel, void* context, WebFetchClock clock) {
  if (!clock) clock = monotonic_ms;
  const auto start = clock(), deadline = start + request.timeout_ms;
  const auto remaining = [&] { return std::max<int64_t>(0, deadline - clock()); };
  const auto cancelled = [&] { return cancel && cancel(context); };
  if (cancelled()) { auto out = error("Web fetch cancelled"); out.cancelled = true; return out; }
  if (!http || !browser || !browser_http_url(request.url, request.allow_local_for_tests) || request.timeout_ms < 1000 || request.timeout_ms > 30000) return error("Invalid public HTTP(S) fetch request");
  if (request.mode != "auto" && request.mode != "http" && request.mode != "browser") return error("Fetch mode must be auto, http or browser");
  const char* configured_proxy = std::getenv("AGENT_BROWSER_PROXY");
  const std::string proxy = configured_proxy ? configured_proxy : "";
  if (!proxy.empty() && !browser_http_url(proxy, true)) return error("Invalid web proxy configuration");
  const auto key = request.url + '\n' + proxy + '\n' + request.mode;
  if (!request.fresh) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto found = cache.find(key);
    if (found != cache.end() && start >= found->second.at && start - found->second.at < kCacheTtl) {
      auto out = found->second.result; out.cache_hit = true; out.cache_age_ms = start - found->second.at; return out;
    }
  }
  BrowserResult result;
  const auto render = [&](const std::string& url) {
    BrowserRequest next = request; next.url = url; next.search = false;
    next.timeout_ms = static_cast<int>(std::min<int64_t>(30000, remaining() - 500));
    if (next.timeout_ms < 1000) { auto out = error("Web fetch deadline exhausted before rendering"); out.timed_out = true; return out; }
    auto out = browser(next, cancel, context); out.source = "local_chrome"; out.rendered = true; return out;
  };
  if (request.mode == "browser") result = render(request.url);
  else {
    std::string url = request.url; size_t consumed = 0;
    for (int redirects = 0;; ++redirects) {
      if (cancelled()) { result = error("Web fetch cancelled"); result.cancelled = true; break; }
      const auto transport_budget = remaining();
      if (transport_budget <= 0) { result = error("Web fetch timed out"); result.timed_out = true; break; }
      auto response = http(url, "GET", {{"Accept", "text/html, text/plain;q=0.9"}, {"Accept-Encoding", "gzip, deflate"}, {"User-Agent", "AgentC/1.0"}}, "",
          transport_budget, kBodyCap - consumed, proxy, nullptr, cancel, context, false);
      result = error("HTTP request failed"); result.http_status = static_cast<int>(response.http_status);
      result.timed_out = response.timed_out; result.cancelled = response.cancelled;
      if (response.cancelled) { result.error = "Web fetch cancelled"; break; }
      if (response.timed_out) { result.error = "Web fetch timed out"; break; }
      if (response.response_too_large || response.response_body.size() > kBodyCap - consumed) { result.error = "Decoded HTTP response exceeded 2 MiB"; break; }
      if (!response.ok) { result.error = "HTTP transport failed"; break; }
      consumed += response.response_body.size();
      if (response.http_status >= 300 && response.http_status <= 399) {
        if (redirects == 3) { result.error = "HTTP redirect limit exceeded"; break; }
        const auto found = response.response_headers.find("location");
        const auto next = found == response.response_headers.end() ? "" : resolve(url, found->second);
        if (!browser_http_url(next, request.allow_local_for_tests)) { result.error = "HTTP redirect destination is not public HTTP(S)"; result.blocked = true; break; }
        if (consumed == kBodyCap) { result.error = "Decoded HTTP response budget exhausted"; break; }
        url = next; continue;
      }
      if (response.http_status < 200 || response.http_status >= 300) {
        result.error = "HTTP page returned status " + std::to_string(response.http_status);
        result.blocked = response.http_status == 401 || response.http_status == 403 || response.http_status == 429; break;
      }
      const auto content_type = lower(response.response_headers["content-type"]);
      const auto type = trim(content_type.substr(0, content_type.find(';')));
      std::string charset;
      const auto charset_at = content_type.find("charset=");
      if (charset_at != std::string::npos) {
        charset = trim(content_type.substr(charset_at + 8));
        charset = trim(charset.substr(0, charset.find(';')));
        if (charset.size() >= 2 && (charset.front() == '\"' || charset.front() == '\'') && charset.back() == charset.front()) charset = charset.substr(1, charset.size() - 2);
        if (charset.empty() || charset.size() > 32 || charset.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos) { result.error = "Unsupported HTTP charset declaration"; break; }
      }
      if (type != "text/html" && type != "application/xhtml+xml" && type != "text/plain") { result.error = "Unsupported HTTP content type"; break; }
      if (type == "text/plain") {
        if ((!charset.empty() && charset != "utf-8" && charset != "utf8" && charset != "us-ascii") || response.response_body.find('\0') != std::string::npos || !xmlCheckUTF8(BAD_CAST response.response_body.c_str())) { result.error = "Plain text response requires valid UTF-8 or ASCII"; break; }
        result.page["url"] = url; result.page["title"] = ""; result.page["content_type"] = "text/plain";
        result.page["text"] = normalize(response.response_body, kTextCap); result.page["text_truncated"] = response.response_body.size() > kTextCap;
        result.page["links"] = Json::Value(Json::arrayValue); result.ok = true; result.error.clear();
      } else {
        auto parsed = extract_html(response.response_body, url, charset, request.allow_local_for_tests, cancel, context);
        result.page = std::move(parsed.page); result.blocked = parsed.blocked; result.cancelled = parsed.cancelled;
        if (!parsed.error.empty()) { result.error = parsed.error; break; }
        if (parsed.shell && request.mode == "auto") result = render(url);
        else { result.ok = true; result.error.clear(); result.page["needs_rendering"] = parsed.shell; }
      }
      break;
    }
  }
  if (cancelled()) { result.ok = false; result.cancelled = true; result.error = "Web fetch cancelled"; }
  if (!remaining() && !result.cancelled) { result.ok = false; result.timed_out = true; result.error = "Web fetch timed out"; }
  if (!result.ok && request.fresh) { std::lock_guard<std::mutex> lock(cache_mutex); cache.erase(key); }
  if (result.ok) {
    result.fetched_at = timestamp(); result.cache_hit = false; result.cache_age_ms = 0;
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    if (Json::writeString(writer, result.page).size() <= 256 * 1024) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      for (auto it = cache.begin(); it != cache.end();) { if (start - it->second.at >= kCacheTtl) it = cache.erase(it); else ++it; }
      if (cache.size() >= 32 && !cache.count(key)) {
        auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
        cache.erase(oldest);
      }
      cache[key] = {result, clock()};
    }
  }
  return result;
}

BrowserResult web_fetch_read_page(const BrowserRequest& request, HostCancelCallback cancel, void* context) {
  return web_fetch_with_transport(request, agentd::http_request, browser_read_page, cancel, context);
}
}  // namespace host_tools_internal
