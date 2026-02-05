#include "runtime_config.h"

#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <map>
#include <sstream>

namespace agentd {
namespace {

static const char* kRuntimeConfigMetaKey = "daemon.runtime_config_json";
static const char* kRuntimeSecretsMetaKey = "daemon.runtime_secrets_json";

static bool is_known_provider(const std::string& provider) {
  return provider == "deepseek" || provider == "openrouter" || provider == "moonshot" || provider == "openai";
}

}  // namespace

bool load_runtime_config_best_effort(AgentDb& db, DaemonConfig* cfg_io, std::string* out_error) {
  RuntimeConfigLoadOptions opt;
  return load_runtime_config_best_effort(db, cfg_io, out_error, opt);
}

bool load_runtime_config_best_effort(
  AgentDb& db,
  DaemonConfig* cfg_io,
  std::string* out_error,
  const RuntimeConfigLoadOptions& opt
) {
  if (out_error) out_error->clear();
  if (!cfg_io) {
    if (out_error) *out_error = "missing cfg_io";
    return false;
  }

  // Non-secret defaults.
  {
    std::string raw;
    std::string err;
    if (!db.meta_get(kRuntimeConfigMetaKey, &raw, &err)) {
      if (!err.empty() && out_error) *out_error = err;
      // Best-effort: treat missing key as "no runtime config".
    } else if (!raw.empty()) {
      Json::Value v;
      if (!json_parse_object(raw, &v, &err)) {
        if (out_error) *out_error = "invalid runtime_config_json: " + err;
        return false;
      }

      if (v.isMember("base_url") && v["base_url"].isString()) cfg_io->base_url = v["base_url"].asString();
      if (v.isMember("model") && v["model"].isString()) cfg_io->model = v["model"].asString();
      if (v.isMember("system_profile") && v["system_profile"].isString()) cfg_io->system_profile = v["system_profile"].asString();
      if (v.isMember("summary_model")) {
        if (v["summary_model"].isNull()) cfg_io->summary_model.clear();
        else if (v["summary_model"].isString()) cfg_io->summary_model = v["summary_model"].asString();
      }
      if (v.isMember("summary_max_chars") && v["summary_max_chars"].isInt64()) {
        const auto n = v["summary_max_chars"].asInt64();
        if (n >= 0) cfg_io->summary_max_chars = (size_t)n;
      }
      if (v.isMember("proxy_url")) {
        if (v["proxy_url"].isNull()) cfg_io->proxy_url.clear();
        else if (v["proxy_url"].isString()) cfg_io->proxy_url = v["proxy_url"].asString();
      }
      if (v.isMember("timeout_ms") && v["timeout_ms"].isInt64()) {
        const auto n = v["timeout_ms"].asInt64();
        if (n > 0) cfg_io->timeout_ms = (long)n;
      }
      if (v.isMember("workflow_admit_max_inflight_tasks_per_session") && v["workflow_admit_max_inflight_tasks_per_session"].isInt()) {
        cfg_io->workflow_admit_max_inflight_tasks_per_session =
          std::max(0, std::min(100000, v["workflow_admit_max_inflight_tasks_per_session"].asInt()));
      }
      if (v.isMember("workflow_admit_max_inflight_tasks_total") && v["workflow_admit_max_inflight_tasks_total"].isInt()) {
        cfg_io->workflow_admit_max_inflight_tasks_total =
          std::max(0, std::min(1000000, v["workflow_admit_max_inflight_tasks_total"].asInt()));
      }

      auto read_string_array = [&](const char* k, std::vector<std::string>* outv) {
        if (!k || !outv) return;
        outv->clear();
        if (!v.isMember(k) || !v[k].isArray()) return;
        for (Json::ArrayIndex i = 0; i < v[k].size(); i++) {
          if (!v[k][i].isString()) continue;
          const std::string s = trim_copy(v[k][i].asString());
          if (!s.empty()) outv->push_back(s);
        }
      };

      if (opt.override_workflow_http_allow_hosts) {
        read_string_array("workflow_http_allow_hosts", &cfg_io->workflow_http_allow_hosts);
      }
      if (opt.override_workflow_http_allow_cidrs) {
        read_string_array("workflow_http_allow_cidrs", &cfg_io->workflow_http_allow_cidrs);
      }
      if (opt.override_workflow_http_deny_cidrs) {
        read_string_array("workflow_http_deny_cidrs", &cfg_io->workflow_http_deny_cidrs);
      }
      if (opt.override_workflow_http_deny_private_addrs) {
        if (v.isMember("workflow_http_deny_private_addrs") && v["workflow_http_deny_private_addrs"].isBool()) {
          cfg_io->workflow_http_deny_private_addrs = v["workflow_http_deny_private_addrs"].asBool();
        }
      }
      if (opt.override_workflow_http_dns_pin) {
        if (v.isMember("workflow_http_dns_pin") && v["workflow_http_dns_pin"].isBool()) {
          cfg_io->workflow_http_dns_pin = v["workflow_http_dns_pin"].asBool();
        }
      }
    }
  }

  // Secrets (provider keys).
  {
    std::string raw;
    std::string err;
    if (!db.meta_get(kRuntimeSecretsMetaKey, &raw, &err)) {
      if (!err.empty() && out_error) *out_error = err;
      return true;  // missing key is fine
    }
    if (raw.empty()) return true;

    Json::Value v;
    if (!json_parse_object(raw, &v, &err)) {
      if (out_error) *out_error = "invalid runtime_secrets_json: " + err;
      return false;
    }

    if (v.isObject()) {
      for (const auto& k : v.getMemberNames()) {
        if (!is_known_provider(k)) continue;
        const Json::Value& val = v[k];
        if (val.isNull()) {
          cfg_io->provider_keys.erase(k);
        } else if (val.isString()) {
          const std::string s = trim_copy(val.asString());
          if (s.empty()) cfg_io->provider_keys.erase(k);
          else cfg_io->provider_keys[k] = s;
        }
      }
    }
  }

  return true;
}

bool save_runtime_config_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error) {
  if (out_error) out_error->clear();
  Json::Value v(Json::objectValue);
  v["base_url"] = cfg.base_url;
  v["model"] = cfg.model;
  v["system_profile"] = cfg.system_profile;
  v["summary_model"] = cfg.summary_model.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.summary_model);
  v["summary_max_chars"] = (Json::UInt64)cfg.summary_max_chars;
  v["proxy_url"] = cfg.proxy_url.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.proxy_url);
  v["timeout_ms"] = (Json::Int64)cfg.timeout_ms;
  v["workflow_admit_max_inflight_tasks_per_session"] = cfg.workflow_admit_max_inflight_tasks_per_session;
  v["workflow_admit_max_inflight_tasks_total"] = cfg.workflow_admit_max_inflight_tasks_total;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.workflow_http_allow_hosts) if (!s.empty()) arr.append(s);
    v["workflow_http_allow_hosts"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.workflow_http_allow_cidrs) if (!s.empty()) arr.append(s);
    v["workflow_http_allow_cidrs"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.workflow_http_deny_cidrs) if (!s.empty()) arr.append(s);
    v["workflow_http_deny_cidrs"] = arr;
  }
  v["workflow_http_deny_private_addrs"] = cfg.workflow_http_deny_private_addrs;
  v["workflow_http_dns_pin"] = cfg.workflow_http_dns_pin;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  const std::string s = Json::writeString(wb, v);
  return db.meta_set(kRuntimeConfigMetaKey, s, out_error);
}

bool save_runtime_secrets_best_effort(AgentDb& db, const DaemonConfig& cfg, std::string* out_error) {
  if (out_error) out_error->clear();

  Json::Value v(Json::objectValue);
  for (const auto& provider : {"deepseek", "openrouter", "moonshot", "openai"}) {
    const auto it = cfg.provider_keys.find(provider);
    if (it == cfg.provider_keys.end() || it->second.empty()) continue;
    v[provider] = it->second;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  const std::string s = Json::writeString(wb, v);
  return db.meta_set(kRuntimeSecretsMetaKey, s, out_error);
}

}  // namespace agentd
