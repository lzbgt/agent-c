#include "openrouter_models_endpoint.h"

#include "http_util.h"
#include "json_util.h"
#include "openrouter_util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentd {
namespace {

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

void handle_openrouter_models_endpoint(
  const OpenAIClientConfig& ocfg,
  bool daemon_auth_enabled,
  const HttpRequest& req,
  HttpResponse* resp
) {
  if (!resp) return;

  // Query params:
  // - min_total/max_total: total $/1M tokens (prompt+completion)
  // - require_multimodal_input: 1/0
  // - require_tools: 1/0
  // - include_free: 1/0
  // - limit: max rows
  // - refresh: bypass cache
  const double min_total = [&]() -> double {
    const auto q = query_get(req.query, "min_total");
    if (!q || q->empty()) return 0.01;
    try { return std::stod(*q); } catch (...) { return 0.01; }
  }();
  const double max_total = [&]() -> double {
    const auto q = query_get(req.query, "max_total");
    if (!q || q->empty()) return 0.50;
    try { return std::stod(*q); } catch (...) { return 0.50; }
  }();
  const bool require_multimodal_input = [&]() -> bool {
    const auto q = query_get(req.query, "require_multimodal_input");
    if (!q) return true;
    return string_to_bool(*q);
  }();
  const bool require_tools = [&]() -> bool {
    const auto q = query_get(req.query, "require_tools");
    if (!q) return true;
    return string_to_bool(*q);
  }();
  const bool include_free = [&]() -> bool {
    const auto q = query_get(req.query, "include_free");
    if (!q) return false;
    return string_to_bool(*q);
  }();
  const int limit = [&]() -> int {
    const auto q = query_get(req.query, "limit");
    if (!q || q->empty()) return 50;
    try { return std::max(1, std::min(200, std::stoi(*q))); } catch (...) { return 50; }
  }();
  const bool refresh = [&]() -> bool {
    const auto q = query_get(req.query, "refresh");
    if (!q) return false;
    return string_to_bool(*q);
  }();

  // Base url for OpenRouter models endpoint.
  const std::string base_url = [&]() -> std::string {
    const auto q = query_get(req.query, "base_url");
    if (q && !q->empty()) return *q;
    const char* b = getenv_s("OPENROUTER_API_BASE");
    return b && b[0] ? std::string(b) : std::string("https://openrouter.ai/api/v1");
  }();
  const std::string models_url = trim_slashes(base_url) + "/models";

  // API key precedence (provider key; distinct from daemon auth):
  // 1) X-OpenRouter-Key header
  // 2) Authorization header (Bearer ...) ONLY when daemon auth is disabled
  // 3) env OPENROUTER_API_KEY
  // 4) env OPENAI_API_KEY (fallback)
  std::string key;
  {
    const std::string xk = header_get_ci(req.headers, "x-openrouter-key");
    if (!xk.empty()) {
      key = xk;
    } else if (!daemon_auth_enabled) {
      const std::string auth = header_get_ci(req.headers, "authorization");
      key = bearer_token_from_auth_header(auth);
    }
    if (key.empty()) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
    }
  }
  if (key.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing OpenRouter key (set OPENROUTER_API_KEY or send X-OpenRouter-Key)\"}";
    return;
  }

  struct Cache {
    std::mutex mu;
    int64_t fetched_unix_ms = 0;
    std::string cache_key;
    Json::Value payload;
  };
  static Cache cache;

  const auto cache_key = [&]() -> std::string {
    const size_t kh = std::hash<std::string>{}(key);
    return models_url + "|" + std::to_string((unsigned long long)kh);
  }();
  const int64_t now = now_unix_ms();
  const int64_t ttl_ms = 10 * 60 * 1000;

  Json::Value payload;
  bool cached = false;
  {
    std::lock_guard<std::mutex> lk(cache.mu);
    if (!refresh && cache.cache_key == cache_key && cache.payload.isObject() && (now - cache.fetched_unix_ms) < ttl_ms) {
      payload = cache.payload;
      cached = true;
    }
  }

  long http_status = 0;
  std::string raw_body;
  if (!cached) {
    OpenAIClientConfig cfg2 = ocfg;
    cfg2.base_url = models_url; // unused by GET
    cfg2.api_key = key;

    OpenAIRawResult r = openai_http_get_raw(cfg2, models_url, {});
    http_status = r.http_status;
    raw_body = r.response_body;
    if (http_status < 200 || http_status >= 300) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to fetch OpenRouter models";
      o["http_status"] = (Json::Int64)http_status;
      o["http_body"] = raw_body;
      resp->status = 502;
      resp->body = json_stringify(o);
      return;
    }
    std::string perr;
    if (!json_parse_any(raw_body, &payload, &perr) || !payload.isObject()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to parse OpenRouter models JSON";
      o["parse_error"] = perr;
      o["http_status"] = (Json::Int64)http_status;
      resp->status = 502;
      resp->body = json_stringify(o);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(cache.mu);
      cache.cache_key = cache_key;
      cache.payload = payload;
      cache.fetched_unix_ms = now;
    }
  }

  const auto& data = payload["data"];
  if (!data.isArray()) {
    resp->status = 502;
    resp->body = "{\"ok\":false,\"error\":\"unexpected OpenRouter models response (missing data array)\"}";
    return;
  }

  struct Row {
    double total = 0.0;
    double prompt = 0.0;
    double completion = 0.0;
    Json::Value model;
  };
  std::vector<Row> rows;
  rows.reserve((size_t)data.size());
  for (const auto& m : data) {
    if (!m.isObject()) continue;
    const auto& pricing = m["pricing"];
    const double prompt_pm = pricing_to_per_million(pricing["prompt"]);
    const double completion_pm = pricing_to_per_million(pricing["completion"]);
    const double total = prompt_pm + completion_pm;
    if (!include_free && total <= 0.0) continue;
    if (total < min_total || total > max_total) continue;
    if (require_tools && !model_supports_tools(m)) continue;
    if (require_multimodal_input && !model_has_multimodal_input(m)) continue;
    Row r;
    r.total = total;
    r.prompt = prompt_pm;
    r.completion = completion_pm;
    r.model = m;
    rows.push_back(std::move(r));
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.total != b.total) return a.total < b.total;
    if (a.prompt != b.prompt) return a.prompt < b.prompt;
    if (a.completion != b.completion) return a.completion < b.completion;
    const std::string ida = a.model.isMember("id") && a.model["id"].isString() ? a.model["id"].asString() : "";
    const std::string idb = b.model.isMember("id") && b.model["id"].isString() ? b.model["id"].asString() : "";
    return ida < idb;
  });

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["source"] = "openrouter";
  out["base_url"] = base_url;
  out["models_url"] = models_url;
  out["cached"] = cached;
  out["fetched_unix_ms"] = (Json::Int64)(cached ? cache.fetched_unix_ms : now);
  out["min_total"] = min_total;
  out["max_total"] = max_total;
  out["require_multimodal_input"] = require_multimodal_input;
  out["require_tools"] = require_tools;
  out["include_free"] = include_free;
  out["limit"] = limit;
  out["total_models"] = (Json::UInt64)data.size();

  Json::Value arr(Json::arrayValue);
  for (int i = 0; i < limit && i < (int)rows.size(); i++) {
    const auto& r = rows[(size_t)i];
    const auto& m = r.model;
    Json::Value e(Json::objectValue);
    e["id"] = m.isMember("id") && m["id"].isString() ? m["id"].asString() : "";
    e["name"] = m.isMember("name") && m["name"].isString() ? m["name"].asString() : "";
    e["context_length"] = m.isMember("context_length") ? m["context_length"] : Json::Value(0);
    e["total_usd_per_million"] = r.total;
    e["prompt_usd_per_million"] = r.prompt;
    e["completion_usd_per_million"] = r.completion;
    e["supports_tools"] = model_supports_tools(m);
    e["supports_multimodal_input"] = model_has_multimodal_input(m);
    const auto& arch = m["architecture"];
    if (arch.isObject()) {
      e["input_modalities"] = arch["input_modalities"];
      e["output_modalities"] = arch["output_modalities"];
    }
    arr.append(e);
  }
  out["count"] = (Json::UInt64)arr.size();
  out["models"] = arr;
  out["recommended_model"] = arr.size() > 0 ? arr[0]["id"] : Json::Value("");

  resp->body = json_stringify(out);
}

}  // namespace agentd

