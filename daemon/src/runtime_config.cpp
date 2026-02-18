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

static size_t clamp_upload_max_bytes(unsigned long long n) {
  const unsigned long long kMax = 512ull * 1024ull * 1024ull;
  if (n > kMax) n = kMax;
  return (size_t)n;
}

static bool is_safe_tool_name(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 128) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static void upsert_tool_call_limit(std::vector<std::pair<std::string, size_t>>* limits, std::string tool, size_t max_calls) {
  if (!limits || tool.empty()) return;
  for (auto& kv : *limits) {
    if (kv.first == tool) {
      kv.second = max_calls;
      return;
    }
  }
  limits->push_back(std::make_pair(std::move(tool), max_calls));
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
      uint64_t n_u64 = 0;
      if (json_get_u64_nonneg(v, "max_steps_default", &n_u64)) {
        cfg_io->max_steps_default = (size_t)n_u64;
      }
      if (json_get_u64_nonneg(v, "max_tool_calls_total_default", &n_u64)) {
        cfg_io->max_tool_calls_total_default = (size_t)n_u64;
      }
      if (json_get_u64_nonneg(v, "max_tool_calls_per_tool_default", &n_u64)) {
        cfg_io->max_tool_calls_per_tool_default = (size_t)n_u64;
      }
      if (json_get_u64_nonneg(v, "max_tool_call_args_chars_default", &n_u64)) {
        cfg_io->max_tool_call_args_chars_default = (size_t)n_u64;
      }
      if (v.isMember("timeout_ms") && v["timeout_ms"].isInt64()) {
        const auto n = v["timeout_ms"].asInt64();
        if (n > 0) cfg_io->timeout_ms = (long)n;
      }
      if (opt.override_upload_max_bytes) {
        if (v.isMember("upload_max_bytes") && v["upload_max_bytes"].isInt64()) {
          auto n = v["upload_max_bytes"].asInt64();
          if (n < 0) n = 0;
          cfg_io->upload_max_bytes = clamp_upload_max_bytes((unsigned long long)n);
        } else if (v.isMember("upload_max_bytes") && v["upload_max_bytes"].isUInt64()) {
          const auto n = v["upload_max_bytes"].asUInt64();
          cfg_io->upload_max_bytes = clamp_upload_max_bytes((unsigned long long)n);
        }
      }
      if (opt.override_blob_store) {
        if (v.isMember("blob_store_mode") && v["blob_store_mode"].isString()) {
          cfg_io->blob_store_mode = v["blob_store_mode"].asString();
        }
        if (v.isMember("blob_store_endpoint") && v["blob_store_endpoint"].isString()) {
          cfg_io->blob_store_endpoint = v["blob_store_endpoint"].asString();
        }
        if (v.isMember("blob_store_region") && v["blob_store_region"].isString()) {
          cfg_io->blob_store_region = v["blob_store_region"].asString();
        }
        if (v.isMember("blob_store_bucket") && v["blob_store_bucket"].isString()) {
          cfg_io->blob_store_bucket = v["blob_store_bucket"].asString();
        }
        if (v.isMember("blob_store_prefix") && v["blob_store_prefix"].isString()) {
          cfg_io->blob_store_prefix = v["blob_store_prefix"].asString();
        }
        if (v.isMember("blob_store_path_style") && v["blob_store_path_style"].isBool()) {
          cfg_io->blob_store_path_style = v["blob_store_path_style"].asBool();
        }
        if (v.isMember("blob_store_read_mode") && v["blob_store_read_mode"].isString()) {
          cfg_io->blob_store_read_mode = v["blob_store_read_mode"].asString();
        }
        if (v.isMember("blob_store_cache_mode") && v["blob_store_cache_mode"].isString()) {
          cfg_io->blob_store_cache_mode = v["blob_store_cache_mode"].asString();
        }
        if (v.isMember("blob_store_cache_max_bytes") && (v["blob_store_cache_max_bytes"].isInt64() || v["blob_store_cache_max_bytes"].isUInt64())) {
          const auto n = v["blob_store_cache_max_bytes"].isInt64()
            ? std::max<int64_t>(0, v["blob_store_cache_max_bytes"].asInt64())
            : (int64_t)v["blob_store_cache_max_bytes"].asUInt64();
          cfg_io->blob_store_cache_max_bytes = clamp_upload_max_bytes((unsigned long long)n);
        }
        if (v.isMember("blob_store_presign_ttl_sec") && (v["blob_store_presign_ttl_sec"].isInt64() || v["blob_store_presign_ttl_sec"].isUInt64())) {
          const auto n = v["blob_store_presign_ttl_sec"].isInt64()
            ? std::max<int64_t>(0, v["blob_store_presign_ttl_sec"].asInt64())
            : (int64_t)v["blob_store_presign_ttl_sec"].asUInt64();
          cfg_io->blob_store_presign_ttl_sec = std::min<int64_t>(604800, n);
        }
        if (v.isMember("blob_store_timeout_ms") && (v["blob_store_timeout_ms"].isInt64() || v["blob_store_timeout_ms"].isUInt64())) {
          const auto n = v["blob_store_timeout_ms"].isInt64()
            ? std::max<int64_t>(0, v["blob_store_timeout_ms"].asInt64())
            : (int64_t)v["blob_store_timeout_ms"].asUInt64();
          cfg_io->blob_store_timeout_ms = std::min<int64_t>(30LL * 60 * 1000, n);
        }
      }
      if (v.isMember("tool_call_limits_default") && v["tool_call_limits_default"].isArray()) {
        cfg_io->tool_call_limits_default.clear();
        const Json::Value arr = v["tool_call_limits_default"];
        for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
          const Json::Value item = arr[i];
          if (!item.isObject()) continue;
          if (!item.isMember("tool") || !item["tool"].isString()) continue;
          const std::string tool = trim_copy(item["tool"].asString());
          if (!is_safe_tool_name(tool)) continue;
          uint64_t max_calls_u64 = 0;
          if (!json_get_u64_nonneg(item, "max_calls", &max_calls_u64)) continue;
          upsert_tool_call_limit(&cfg_io->tool_call_limits_default, tool, (size_t)max_calls_u64);
        }
      }
      if (v.isMember("edge_auth_required") && v["edge_auth_required"].isBool()) {
        cfg_io->edge_auth_required = v["edge_auth_required"].asBool();
      }
      if (v.isMember("edge_auth_require_ts") && v["edge_auth_require_ts"].isBool()) {
        cfg_io->edge_auth_require_ts = v["edge_auth_require_ts"].asBool();
      }
      if (v.isMember("edge_auth_max_skew_ms") && v["edge_auth_max_skew_ms"].isInt64()) {
        const auto n = v["edge_auth_max_skew_ms"].asInt64();
        cfg_io->edge_auth_max_skew_ms = std::max<int64_t>(0, std::min<int64_t>(30LL * 24 * 60 * 60 * 1000, n));
      } else if (v.isMember("edge_auth_max_skew_ms") && v["edge_auth_max_skew_ms"].isUInt64()) {
        const auto n = (int64_t)v["edge_auth_max_skew_ms"].asUInt64();
        cfg_io->edge_auth_max_skew_ms = std::max<int64_t>(0, std::min<int64_t>(30LL * 24 * 60 * 60 * 1000, n));
      }
      if (v.isMember("edge_auth_require_seq") && v["edge_auth_require_seq"].isBool()) {
        cfg_io->edge_auth_require_seq = v["edge_auth_require_seq"].asBool();
      }
      if (v.isMember("edge_auth_kid_policy") && v["edge_auth_kid_policy"].isString()) {
        const std::string s = trim_copy(v["edge_auth_kid_policy"].asString());
        if (s == "any" || s == "match_node" || s == "node_prefix") cfg_io->edge_auth_kid_policy = s;
      }
      if (v.isMember("edge_attest_required") && v["edge_attest_required"].isBool()) {
        cfg_io->edge_attest_required = v["edge_attest_required"].asBool();
      }
      if (v.isMember("edge_attest_require_sig") && v["edge_attest_require_sig"].isBool()) {
        cfg_io->edge_attest_require_sig = v["edge_attest_require_sig"].asBool();
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

      // Edge auth keyring (kid -> secret).
      if (v.isMember("edge_auth_hmac_keys") && v["edge_auth_hmac_keys"].isObject()) {
        cfg_io->edge_auth_hmac_keys.clear();
        const Json::Value& ek = v["edge_auth_hmac_keys"];
        for (const auto& kid : ek.getMemberNames()) {
          const Json::Value& kv = ek[kid];
          if (!kv.isString()) continue;
          const std::string s = trim_copy(kv.asString());
          if (s.empty()) continue;
          cfg_io->edge_auth_hmac_keys[kid] = s;
        }
      }

      // Edge auth pubkey directory (kid -> base64(pubkey32)).
      if (v.isMember("edge_auth_ed25519_pubkeys") && v["edge_auth_ed25519_pubkeys"].isObject()) {
        cfg_io->edge_auth_ed25519_pubkeys.clear();
        const Json::Value& ek = v["edge_auth_ed25519_pubkeys"];
        for (const auto& kid : ek.getMemberNames()) {
          const Json::Value& kv = ek[kid];
          if (!kv.isString()) continue;
          const std::string s = trim_copy(kv.asString());
          if (s.empty()) continue;
          cfg_io->edge_auth_ed25519_pubkeys[kid] = s;
        }
      }

      // Blob store secrets (object store credentials).
      if (v.isMember("blob_store") && v["blob_store"].isObject()) {
        const Json::Value& bs = v["blob_store"];
        if (bs.isMember("access_key") && bs["access_key"].isString()) {
          cfg_io->blob_store_access_key = trim_copy(bs["access_key"].asString());
        }
        if (bs.isMember("secret_key") && bs["secret_key"].isString()) {
          cfg_io->blob_store_secret_key = trim_copy(bs["secret_key"].asString());
        }
        if (bs.isMember("session_token") && bs["session_token"].isString()) {
          cfg_io->blob_store_session_token = trim_copy(bs["session_token"].asString());
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
  v["max_steps_default"] = (Json::UInt64)cfg.max_steps_default;
  v["max_tool_calls_total_default"] = (Json::UInt64)cfg.max_tool_calls_total_default;
  v["max_tool_calls_per_tool_default"] = (Json::UInt64)cfg.max_tool_calls_per_tool_default;
  v["max_tool_call_args_chars_default"] = (Json::UInt64)cfg.max_tool_call_args_chars_default;
  v["timeout_ms"] = (Json::Int64)cfg.timeout_ms;
  v["upload_max_bytes"] = (Json::UInt64)cfg.upload_max_bytes;
  v["blob_store_mode"] = cfg.blob_store_mode;
  v["blob_store_endpoint"] = cfg.blob_store_endpoint.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.blob_store_endpoint);
  v["blob_store_region"] = cfg.blob_store_region;
  v["blob_store_bucket"] = cfg.blob_store_bucket.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.blob_store_bucket);
  v["blob_store_prefix"] = cfg.blob_store_prefix;
  v["blob_store_path_style"] = cfg.blob_store_path_style;
  v["blob_store_read_mode"] = cfg.blob_store_read_mode;
  v["blob_store_cache_mode"] = cfg.blob_store_cache_mode;
  v["blob_store_cache_max_bytes"] = (Json::UInt64)cfg.blob_store_cache_max_bytes;
  v["blob_store_presign_ttl_sec"] = (Json::Int64)cfg.blob_store_presign_ttl_sec;
  v["blob_store_timeout_ms"] = (Json::Int64)cfg.blob_store_timeout_ms;
  v["edge_auth_required"] = cfg.edge_auth_required;
  v["edge_auth_require_ts"] = cfg.edge_auth_require_ts;
  v["edge_auth_max_skew_ms"] = (Json::Int64)cfg.edge_auth_max_skew_ms;
  v["edge_auth_require_seq"] = cfg.edge_auth_require_seq;
  v["edge_auth_kid_policy"] = cfg.edge_auth_kid_policy;
  v["edge_attest_required"] = cfg.edge_attest_required;
  v["edge_attest_require_sig"] = cfg.edge_attest_require_sig;
  v["workflow_admit_max_inflight_tasks_per_session"] = cfg.workflow_admit_max_inflight_tasks_per_session;
  v["workflow_admit_max_inflight_tasks_total"] = cfg.workflow_admit_max_inflight_tasks_total;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& s : cfg.workflow_http_allow_hosts) if (!s.empty()) arr.append(s);
    v["workflow_http_allow_hosts"] = arr;
  }
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : cfg.tool_call_limits_default) {
      Json::Value item(Json::objectValue);
      item["tool"] = p.first;
      item["max_calls"] = (Json::UInt64)p.second;
      arr.append(item);
    }
    v["tool_call_limits_default"] = arr;
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
  if (!cfg.edge_auth_hmac_keys.empty()) {
    Json::Value ek(Json::objectValue);
    for (const auto& p : cfg.edge_auth_hmac_keys) {
      if (p.first.empty() || p.second.empty()) continue;
      ek[p.first] = p.second;
    }
    if (!ek.empty()) v["edge_auth_hmac_keys"] = ek;
  }
  if (!cfg.edge_auth_ed25519_pubkeys.empty()) {
    Json::Value ek(Json::objectValue);
    for (const auto& p : cfg.edge_auth_ed25519_pubkeys) {
      if (p.first.empty() || p.second.empty()) continue;
      ek[p.first] = p.second;
    }
    if (!ek.empty()) v["edge_auth_ed25519_pubkeys"] = ek;
  }
  if (!cfg.blob_store_access_key.empty() || !cfg.blob_store_secret_key.empty() || !cfg.blob_store_session_token.empty()) {
    Json::Value bs(Json::objectValue);
    if (!cfg.blob_store_access_key.empty()) bs["access_key"] = cfg.blob_store_access_key;
    if (!cfg.blob_store_secret_key.empty()) bs["secret_key"] = cfg.blob_store_secret_key;
    if (!cfg.blob_store_session_token.empty()) bs["session_token"] = cfg.blob_store_session_token;
    v["blob_store"] = bs;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  const std::string s = Json::writeString(wb, v);
  return db.meta_set(kRuntimeSecretsMetaKey, s, out_error);
}

}  // namespace agentd
