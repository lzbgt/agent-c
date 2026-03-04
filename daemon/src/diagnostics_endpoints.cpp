#include "diagnostics_endpoints.h"

#include "agent_db.h"
#include "daemon_auth.h"
#include "json_util.h"
#include "mount_allowlist.h"
#include "provider_util.h"
#include "run_endpoints.h"
#include "secrets_file.h"
#include "string_util.h"
#include "tool_extension.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace agentd {
namespace {

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count();
}

static int64_t uptime_ms(std::chrono::steady_clock::time_point start_time) {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - start_time)
           .count();
}

static Json::Value base_diag(std::chrono::steady_clock::time_point start_time) {
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["service"] = "agentd";
  root["version"] = "0.1";
  root["now_unix_ms"] = Json::Int64(now_unix_ms());
  root["uptime_ms"] = Json::Int64(uptime_ms(start_time));
  return root;
}

static std::string default_base_url_for_provider(const std::string& provider) {
  if (provider == "deepseek") return "https://api.deepseek.com";
  if (provider == "moonshot") return "https://api.moonshot.cn/v1";
  if (provider == "openrouter") return "https://openrouter.ai/api/v1";
  if (provider == "openai") return "https://api.openai.com/v1";
  return "";
}

static std::string default_model_for_provider(const std::string& provider) {
  if (provider == "deepseek") return "deepseek-reasoner";
  if (provider == "moonshot") return "kimi-k2.5";
  return "";
}

struct ProviderKeyStatus {
  bool present = false;
  std::string source_kind;
  std::string source_label;
};

static ProviderKeyStatus provider_key_status(const DaemonConfig& cfg, const std::string& provider) {
  ProviderKeyStatus st;
  const auto it = cfg.provider_keys.find(provider);
  if (it != cfg.provider_keys.end() && !it->second.empty()) {
    st.present = true;
    st.source_kind = "config";
    st.source_label = "provider_keys";
    return st;
  }
  if (provider_from_base_url(cfg.base_url) == provider && !cfg.api_key.empty()) {
    st.present = true;
    st.source_kind = "config";
    st.source_label = "api_key";
    return st;
  }

  auto set_env = [&](const char* env_name) {
    if (st.present) return;
    const char* v = getenv_s(env_name);
    if (v && v[0]) {
      st.present = true;
      st.source_kind = "env";
      st.source_label = env_name;
    }
  };

  if (provider == "deepseek") {
    set_env("DEEPSEEK_API_KEY");
  } else if (provider == "moonshot") {
    set_env("KIMI_API_KEY_CN");
    set_env("MOONSHOT_API_KEY");
    set_env("MOONSHOT_API_KEY_CN");
  } else if (provider == "openrouter") {
    set_env("OPENROUTER_API_KEY");
  } else if (provider == "openai") {
    set_env("OPENAI_API_KEY");
  }

  if (st.present) return st;
  if (auto info = load_provider_key_source_best_effort(provider)) {
    st.present = true;
    st.source_kind = info->source_kind;
    st.source_label = info->source_label;
  }
  return st;
}

static std::string env_base_url_for_provider(const std::string& provider) {
  if (provider == "deepseek") {
    if (const char* v = getenv_s("DEEPSEEK_API_BASE")) return v;
  } else if (provider == "moonshot") {
    if (const char* v = getenv_s("MOONSHOT_API_BASE")) return v;
  } else if (provider == "openrouter") {
    if (const char* v = getenv_s("OPENROUTER_API_BASE")) return v;
  } else if (provider == "openai") {
    if (const char* v = getenv_s("OPENAI_API_BASE")) return v;
    if (const char* v2 = getenv_s("OPENAI_BASE_URL")) return v2;
  }
  return "";
}

static std::string provider_key_hint(const std::string& provider) {
  if (provider == "deepseek") return "DEEPSEEK_API_KEY or provider_keys";
  if (provider == "moonshot") return "KIMI_API_KEY_CN or MOONSHOT_API_KEY (or provider_keys)";
  if (provider == "openrouter") return "OPENROUTER_API_KEY or provider_keys";
  if (provider == "openai") return "OPENAI_API_KEY or provider_keys";
  return "";
}

static std::string provider_base_url_env_hint(const std::string& provider) {
  if (provider == "deepseek") return "DEEPSEEK_API_BASE";
  if (provider == "moonshot") return "MOONSHOT_API_BASE";
  if (provider == "openrouter") return "OPENROUTER_API_BASE";
  if (provider == "openai") return "OPENAI_API_BASE";
  return "";
}

}  // namespace

void handle_diagnostics_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
) {
  if (!resp) return;
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value root = base_diag(start_time);
  const std::string active_provider = provider_from_base_url(cfg.base_url);
  root["active_provider"] = active_provider;
  ProviderKeyStatus active_key = provider_key_status(cfg, active_provider);
  root["active_provider_key_present"] = active_key.present;
  if (active_key.present) {
    Json::Value src(Json::objectValue);
    src["kind"] = active_key.source_kind;
    src["label"] = active_key.source_label;
    root["active_provider_key_source"] = src;
  }
  {
    std::string base_url;
    std::string base_url_source;
    if (!cfg.base_url.empty()) {
      base_url = cfg.base_url;
      base_url_source = "config";
    } else {
      base_url = env_base_url_for_provider(active_provider);
      if (!base_url.empty()) base_url_source = "env";
    }
    if (base_url.empty()) {
      base_url = default_base_url_for_provider(active_provider);
      if (!base_url.empty()) base_url_source = "default";
    }
    if (!base_url.empty()) {
      root["active_provider_base_url"] = base_url;
      if (!base_url_source.empty()) root["active_provider_base_url_source"] = base_url_source;
    }
  }
  const bool db_open = db_or_null && db_or_null->is_open();
  root["ready"] = db_open;
  Json::Value checks(Json::objectValue);
  checks["db_open"] = db_open;
  root["checks"] = checks;

  Json::Value errors(Json::arrayValue);

  {
    const auto allow = mount_allowlist_status();
    Json::Value allow_json(Json::objectValue);
    allow_json["path"] = allow.path;
    allow_json["present"] = allow.present;
    allow_json["loaded"] = allow.loaded;
    allow_json["allowed_roots"] = Json::UInt64(allow.allowed_roots);
    allow_json["blocked_patterns"] = Json::UInt64(allow.blocked_patterns);
    if (!allow.error.empty()) allow_json["error"] = allow.error;
    root["sandbox_mount_allowlist"] = allow_json;
  }

  Json::Value db(Json::objectValue);
  if (db_or_null) {
    db["path"] = db_or_null->path();
    std::error_code ec;
    const auto size = std::filesystem::file_size(db_or_null->path(), ec);
    if (!ec) db["size_bytes"] = (Json::UInt64)size;
  }

  if (db_open) {
    AgentDb::TableCounts tables;
    std::string terr;
    if (db_or_null->get_table_counts(&tables, &terr)) {
      Json::Value t(Json::objectValue);
      t["sessions"] = (Json::Int64)tables.sessions;
      t["messages"] = (Json::Int64)tables.messages;
      t["runs"] = (Json::Int64)tables.runs;
      t["events"] = (Json::Int64)tables.events;
      t["artifacts"] = (Json::Int64)tables.artifacts;
      t["ui_actions"] = (Json::Int64)tables.ui_actions;
      t["client_events"] = (Json::Int64)tables.client_events;
      t["audit_records"] = (Json::Int64)tables.audit_records;
      t["jobs"] = (Json::Int64)tables.jobs;
      t["workflows"] = (Json::Int64)tables.workflows;
      t["workflow_tasks"] = (Json::Int64)tables.workflow_tasks;
      t["workflow_events"] = (Json::Int64)tables.workflow_events;
      t["edge_nodes"] = (Json::Int64)tables.edge_nodes;
      t["edge_tasks"] = (Json::Int64)tables.edge_tasks;
      t["edge_workflows"] = (Json::Int64)tables.edge_workflows;
      t["blob_manifest"] = (Json::Int64)tables.blob_manifest;
      t["artifact_blobs"] = (Json::Int64)tables.artifact_blobs;
      db["tables"] = t;
    } else if (!terr.empty()) {
      errors.append("db table counts: " + terr);
    }

    AgentDb::JobStatusCounts jobs;
    std::string jerr;
    if (db_or_null->get_job_status_counts(&jobs, &jerr)) {
      Json::Value jb(Json::objectValue);
      Json::Value by(Json::objectValue);
      for (const auto& kv : jobs.by_status) {
        by[kv.first] = (Json::Int64)kv.second;
      }
      jb["total"] = (Json::Int64)jobs.total;
      jb["by_status"] = by;
      root["jobs"] = jb;
    } else if (!jerr.empty()) {
      errors.append("job counts: " + jerr);
    }

    AgentDb::WorkflowSchedulerStats ws;
    std::string werr;
    if (db_or_null->get_workflow_scheduler_stats(now_unix_ms(), &ws, &werr)) {
      Json::Value wf(Json::objectValue);
      Json::Value by_status(Json::objectValue);
      for (const auto& kv : ws.workflows_by_status) {
        by_status[kv.first] = (Json::Int64)kv.second;
      }
      Json::Value task_by(Json::objectValue);
      for (const auto& kv : ws.tasks_by_status) {
        task_by[kv.first] = (Json::Int64)kv.second;
      }
      wf["workflows_by_status"] = by_status;
      wf["tasks_by_status"] = task_by;
      wf["tasks_queued_ready"] = (Json::Int64)ws.tasks_queued_ready;
      wf["tasks_queued_not_ready"] = (Json::Int64)ws.tasks_queued_not_ready;
      root["workflows"] = wf;
    } else if (!werr.empty()) {
      errors.append("workflow stats: " + werr);
    }
  }

  root["db"] = db;
  if (!errors.empty()) root["warnings"] = errors;

  resp->status = 200;
  resp->body = json_stringify(root);
}

void handle_diagnostics_providers_endpoint(
  const DaemonConfig& cfg,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
) {
  if (!resp) return;
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value root = base_diag(start_time);
  Json::Value providers(Json::objectValue);

  const std::string active_provider = provider_from_base_url(cfg.base_url);
  const std::string providers_list[] = {"deepseek", "moonshot", "openrouter", "openai"};
  for (const auto& p : providers_list) {
    Json::Value pd(Json::objectValue);
    pd["active"] = (active_provider == p);
    ProviderKeyStatus ks = provider_key_status(cfg, p);
    pd["key_present"] = ks.present;
    if (ks.present) {
      Json::Value src(Json::objectValue);
      src["kind"] = ks.source_kind;
      src["label"] = ks.source_label;
      pd["source"] = src;
    }

    std::string base_url;
    std::string base_url_source;
    if (active_provider == p && !cfg.base_url.empty()) {
      base_url = cfg.base_url;
      base_url_source = "config";
    } else {
      base_url = env_base_url_for_provider(p);
      if (!base_url.empty()) base_url_source = "env";
    }
    if (base_url.empty()) {
      base_url = default_base_url_for_provider(p);
      if (!base_url.empty()) base_url_source = "default";
    }
    if (!base_url.empty()) {
      pd["base_url"] = base_url;
      if (!base_url_source.empty()) pd["base_url_source"] = base_url_source;
    }

    if (active_provider == p && !cfg.model.empty()) {
      pd["model"] = cfg.model;
    }
    const std::string def_model = default_model_for_provider(p);
    if (!def_model.empty()) pd["model_default"] = def_model;

    if (p == "moonshot" && ks.present) {
      const bool active = active_provider == "moonshot";
      const bool has_env_base = !env_base_url_for_provider("moonshot").empty();
      if (!active && cfg.base_url.empty() && !has_env_base) {
        pd["warning"] =
          "Moonshot key detected but base_url not configured; set --base-url https://api.moonshot.cn/v1 or MOONSHOT_API_BASE.";
      }
    }

    if (p == "openrouter" && !ks.present) {
      const bool moonshot_present = provider_key_status(cfg, "moonshot").present;
      if (moonshot_present) {
        pd["warning"] = "Moonshot key detected; OpenRouter requires OPENROUTER_API_KEY.";
      }
    }

    if (!pd.isMember("warning") && ks.present && active_provider != p && cfg.base_url.empty() &&
        env_base_url_for_provider(p).empty()) {
      const std::string def_base = default_base_url_for_provider(p);
      const std::string env_hint = provider_base_url_env_hint(p);
      if (!def_base.empty() && !env_hint.empty()) {
        pd["warning"] = std::string("Key detected but base_url not configured; set --base-url ") + def_base + " or " +
          env_hint + ".";
      } else if (!def_base.empty()) {
        pd["warning"] = std::string("Key detected but base_url not configured; set --base-url ") + def_base + ".";
      } else if (!env_hint.empty()) {
        pd["warning"] = std::string("Key detected but base_url not configured; set ") + env_hint + ".";
      } else {
        pd["warning"] = "Key detected but base_url not configured.";
      }
    }

    if (active_provider == p && !ks.present && !pd.isMember("warning")) {
      const std::string hint = provider_key_hint(p);
      if (!hint.empty()) {
        pd["warning"] = "Active provider key missing; set " + hint + ".";
      } else {
        pd["warning"] = "Active provider key missing.";
      }
    }

    providers[p] = pd;
  }

  root["providers"] = providers;
  resp->status = 200;
  resp->body = json_stringify(root);
}

void handle_diagnostics_provider_test_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  if (!resp) return;
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = perr.empty() ? "invalid JSON body" : perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string provider = args.isMember("provider") && args["provider"].isString() ? trim_copy(args["provider"].asString()) : "";
  if (provider.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing provider";
    resp->body = json_stringify(o);
    return;
  }

  std::string base_url = args.isMember("base_url") && args["base_url"].isString()
                           ? trim_copy(args["base_url"].asString())
                           : "";
  if (base_url.empty()) {
    const std::string active_provider = provider_from_base_url(cfg.base_url);
    if (active_provider == provider && !cfg.base_url.empty()) {
      base_url = cfg.base_url;
    } else {
      base_url = env_base_url_for_provider(provider);
    }
  }
  if (base_url.empty()) base_url = default_base_url_for_provider(provider);
  if (base_url.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing base_url for provider";
    resp->body = json_stringify(o);
    return;
  }

  std::string model = args.isMember("model") && args["model"].isString() ? trim_copy(args["model"].asString()) : "";
  if (model.empty()) {
    if (provider_from_base_url(cfg.base_url) == provider && !cfg.model.empty()) {
      model = cfg.model;
    } else {
      model = default_model_for_provider(provider);
    }
  }
  if (model.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing model for provider";
    resp->body = json_stringify(o);
    return;
  }

  const std::string prompt = args.isMember("prompt") && args["prompt"].isString()
                               ? args["prompt"].asString()
                               : "Return exactly: OK";
  const std::string expect = args.isMember("expect") && args["expect"].isString()
                               ? args["expect"].asString()
                               : "OK";

  std::string tools = args.isMember("tools") && args["tools"].isString() ? trim_copy(args["tools"].asString()) : "none";
  if (tools != "none" && tools != "basic" && tools != "host") {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid tools (expected: none|basic|host)";
    resp->body = json_stringify(o);
    return;
  }
  const bool require_tool_call = args.isMember("require_tool_call") && args["require_tool_call"].isBool() ? args["require_tool_call"].asBool() : false;

  long timeout_ms = ocfg.timeout_ms;
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const auto n = args["timeout_ms"].asInt64();
    if (n > 0) timeout_ms = (long)n;
  }

  Json::Value run_req(Json::objectValue);
  run_req["prompt"] = prompt;
  run_req["no_session"] = true;
  run_req["tools"] = tools;
  if (require_tool_call) run_req["require_tool_call"] = true;
  run_req["model"] = model;
  run_req["base_url"] = base_url;
  run_req["timeout_ms"] = (Json::Int64)timeout_ms;

  uint64_t n_u64 = 0;
  if (json_get_u64_nonneg(args, "max_steps", &n_u64)) run_req["max_steps"] = (Json::UInt64)n_u64;
  if (json_get_u64_nonneg(args, "max_tool_calls_total", &n_u64)) run_req["max_tool_calls_total"] = (Json::UInt64)n_u64;
  if (json_get_u64_nonneg(args, "max_tool_calls_per_tool", &n_u64)) run_req["max_tool_calls_per_tool"] = (Json::UInt64)n_u64;
  if (json_get_u64_nonneg(args, "max_tool_call_args_chars", &n_u64)) run_req["max_tool_call_args_chars"] = (Json::UInt64)n_u64;
  if (json_get_u64_nonneg(args, "max_tool_result_chars", &n_u64)) run_req["max_tool_result_chars"] = (Json::UInt64)n_u64;
  if (json_get_u64_nonneg(args, "max_repeated_tool_calls", &n_u64)) run_req["max_repeated_tool_calls"] = (Json::UInt64)n_u64;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string body = Json::writeString(wb, run_req);

  const auto t0 = std::chrono::steady_clock::now();
  Json::Value run_resp = run_request_to_json_internal(cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, body, nullptr);
  const auto t1 = std::chrono::steady_clock::now();
  const int64_t dur_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  const bool run_ok = run_resp.isObject() && run_resp.isMember("ok") && run_resp["ok"].isBool() && run_resp["ok"].asBool();
  std::string assistant_text;
  if (run_resp.isObject() && run_resp.isMember("assistant_text") && run_resp["assistant_text"].isString()) {
    assistant_text = run_resp["assistant_text"].asString();
  }
  const std::string trimmed = trim_copy(assistant_text);
  const bool match = expect.empty() ? true : (trimmed == trim_copy(expect));
  const bool ok = run_ok && match;

  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["provider"] = provider;
  out["base_url"] = base_url;
  out["model"] = model;
  out["duration_ms"] = (Json::Int64)dur_ms;
  if (!expect.empty()) out["expect"] = expect;
  if (!assistant_text.empty()) {
    const size_t kMax = 2048;
    out["assistant_text"] = assistant_text.size() <= kMax ? assistant_text : (assistant_text.substr(0, kMax) + "...");
  }

  if (!run_ok) {
    if (run_resp.isObject() && run_resp.isMember("error") && run_resp["error"].isString()) {
      out["error"] = run_resp["error"].asString();
    } else {
      out["error"] = "provider test failed";
    }
  } else if (!match) {
    out["error"] = "unexpected assistant_text";
  }

  if (run_resp.isObject() && run_resp.isMember("http_status") && run_resp["http_status"].isInt64()) {
    out["http_status"] = run_resp["http_status"];
  }

  if (args.isMember("include_run") && args["include_run"].isBool() && args["include_run"].asBool()) {
    out["run"] = run_resp;
  }

  int status = 200;
  if (run_resp.isObject() && run_resp.isMember("rpc_status") && run_resp["rpc_status"].isInt()) {
    status = run_resp["rpc_status"].asInt();
  }
  if (!ok && status < 400) status = 502;
  resp->status = status;
  resp->body = json_stringify(out);
}

}  // namespace agentd
