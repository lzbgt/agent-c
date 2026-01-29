#include "http_server.h"

#include "agent/agent.h"
#include "agent/provider.h"
#include "agent/runner.h"

#include "openai_client.h"
#include "session_store.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  return std::filesystem::current_path().string();
}

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

static bool json_parse_object(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  *out = v;
  return true;
}
#endif

struct DaemonConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8123;
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string model = "gpt-4o-mini";
  std::string tools = "host";     // none|basic|host
  std::string tools_root = "";    // empty => CWD (unrestricted file edits)
  std::string host_scope_root;    // default: daemon process CWD (for "@host" tool root mode)
  bool yolo_default = true;       // default to unrestricted unless client requests scoped mode
};

static void add_cors(HttpResponse* resp) {
  // Localhost-friendly defaults for a local web UI dev server.
  resp->headers["Access-Control-Allow-Origin"] = "*";
  resp->headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
  resp->headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
}

static std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      auto hex = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return 10 + (x - 'a');
        if (x >= 'A' && x <= 'F') return 10 + (x - 'A');
        return -1;
      };
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((char)((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

static std::optional<std::string> query_get(const std::string& query, const std::string& key) {
  size_t start = 0;
  while (start <= query.size()) {
    size_t amp = query.find('&', start);
    if (amp == std::string::npos) amp = query.size();
    const std::string_view part(query.data() + start, amp - start);
    const size_t eq = part.find('=');
    std::string_view k = eq == std::string_view::npos ? part : part.substr(0, eq);
    std::string_view v = eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1);
    if (k == key) {
      return url_decode(v);
    }
    start = amp + 1;
  }
  return std::nullopt;
}

static std::string content_type_from_path(const std::filesystem::path& p) {
  const std::string ext = p.extension().string();
  auto eqi = [&](const char* s) {
    if (ext.size() != std::strlen(s)) return false;
    for (size_t i = 0; i < ext.size(); i++) {
      if (std::tolower((unsigned char)ext[i]) != std::tolower((unsigned char)s[i])) return false;
    }
    return true;
  };
  if (eqi(".png")) return "image/png";
  if (eqi(".jpg") || eqi(".jpeg")) return "image/jpeg";
  if (eqi(".gif")) return "image/gif";
  if (eqi(".webp")) return "image/webp";
  if (eqi(".svg")) return "image/svg+xml";
  if (eqi(".mp3")) return "audio/mpeg";
  if (eqi(".wav")) return "audio/wav";
  if (eqi(".mp4")) return "video/mp4";
  if (eqi(".webm")) return "video/webm";
  if (eqi(".mov")) return "video/quicktime";
  if (eqi(".txt") || eqi(".md")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

struct ProviderCtx {
  OpenAIClientConfig cfg;
  long last_http_status = 0;
  std::string last_body;
  std::string last_request_body;
  std::string last_error;
};

static agent_status_t provider_generate(void* vctx, const agent_generate_request_t* req, agent_generate_response_t* out_resp) {
  if (!vctx || !req || !out_resp) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  auto* ctx = static_cast<ProviderCtx*>(vctx);
  ctx->last_http_status = 0;
  ctx->last_body.clear();
  ctx->last_request_body.clear();
  ctx->last_error.clear();

  OpenAIClientConfig cfg = ctx->cfg;
  if (req->model && req->model[0]) {
    cfg.model = req->model;
  }

#if defined(AGENT_HAVE_JSONCPP)
  {
    Json::Value root(Json::objectValue);
    root["model"] = cfg.model;
    root["stream"] = false;
    Json::Value messages(Json::arrayValue);
    for (size_t i = 0; i < req->message_count; i++) {
      Json::Value m(Json::objectValue);
      m["role"] = agent_role_to_string(req->messages[i].role);
      m["content"] = std::string(req->messages[i].content, req->messages[i].content_len);
      messages.append(m);
    }
    root["messages"] = messages;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    ctx->last_request_body = Json::writeString(wb, root);
  }
#endif

  const OpenAIChatResult r = openai_chat_completions(cfg, req->messages, req->message_count);
  ctx->last_http_status = r.http_status;
  ctx->last_body = r.response_body;
  ctx->last_error = r.error_message;

  if (r.http_status < 200 || r.http_status >= 300) {
    if (ctx->last_error.empty()) {
      ctx->last_error = openai_format_http_error(r.http_status, r.response_body);
    }
    return AGENT_ERR_INTERNAL;
  }
  if (r.assistant_text.empty()) {
    ctx->last_error = "failed to extract assistant text from response";
    return AGENT_ERR_INTERNAL;
  }
  return agent_string_set_copy(&out_resp->assistant_text, r.assistant_text.c_str(), r.assistant_text.size());
}

int main(int argc, char** argv) {
  DaemonConfig cfg;
  cfg.host_scope_root = std::filesystem::current_path().string();
  // Minimal flag parsing (daemon is host-only; core remains argv/env-free).
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto take = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (a == "--host") {
      if (!take(&cfg.listen_host)) {
        std::cerr << "Missing value for --host\n";
        return 2;
      }
    } else if (a == "--port") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --port\n";
        return 2;
      }
      try {
        const unsigned long p = std::stoul(v);
        cfg.listen_port = (uint16_t)p;
      } catch (...) {
        std::cerr << "Invalid --port\n";
        return 2;
      }
    } else if (a == "--model") {
      if (!take(&cfg.model)) {
        std::cerr << "Missing value for --model\n";
        return 2;
      }
    } else if (a == "--base-url") {
      if (!take(&cfg.base_url)) {
        std::cerr << "Missing value for --base-url\n";
        return 2;
      }
    } else if (a == "--api-key") {
      if (!take(&cfg.api_key)) {
        std::cerr << "Missing value for --api-key\n";
        return 2;
      }
    } else if (a == "--tools") {
      if (!take(&cfg.tools)) {
        std::cerr << "Missing value for --tools\n";
        return 2;
      }
    } else if (a == "--tools-root") {
      if (!take(&cfg.tools_root)) {
        std::cerr << "Missing value for --tools-root\n";
        return 2;
      }
    } else if (a == "--host-scope") {
      if (!take(&cfg.host_scope_root)) {
        std::cerr << "Missing value for --host-scope\n";
        return 2;
      }
    } else if (a == "--yolo") {
      cfg.yolo_default = true;
    } else if (a == "--no-yolo") {
      cfg.yolo_default = false;
    } else if (a == "--help" || a == "-h") {
      std::cerr
        << "Usage: agentd [options]\n"
        << "  --host <ip>          Listen host (default: 127.0.0.1)\n"
        << "  --port <n>           Listen port (default: 8123)\n"
        << "  --model <name>       Default model\n"
        << "  --base-url <url>     Default base url\n"
        << "  --api-key <key>      Default API key (else env)\n"
        << "  --tools host|basic|none   Default toolset (default: host)\n"
        << "  --tools-root <path>  Root/working dir for file edits (default: unrestricted)\n"
        << "  --host-scope <path>  Host scope root for tools_root=\"@host\" (default: current dir)\n"
        << "  --yolo / --no-yolo   Default unrestricted mode (default: yolo)\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 2;
    }
  }

  if (const char* k = getenv_s("OPENAI_API_KEY")) {
    cfg.api_key = k;
  } else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) {
    cfg.api_key = k2;
  } else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) {
    cfg.api_key = k3;
  }
  if (const char* b = getenv_s("OPENAI_API_BASE")) {
    cfg.base_url = b;
  } else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) {
    cfg.base_url = b2;
  } else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) {
    cfg.base_url = b3;
  } else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) {
    cfg.base_url = b4;
  }
  if (const char* m = getenv_s("AGENT_MODEL")) {
    cfg.model = m;
  }

  OpenAIClientConfig ocfg;
  ocfg.base_url = cfg.base_url;
  ocfg.api_key = cfg.api_key;
  ocfg.model = cfg.model;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    ocfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    ocfg.openrouter_x_title = t;
  }

  HttpServer server;
  server.set_default_headers({
    {"Server", "agentd/0.1"},
    {"Access-Control-Allow-Origin", "*"},
    {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
  });

  server.handle("GET", "/api/v1/health", [&](const HttpRequest&, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":true,"service":"agentd","version":"0.1"})";
  });

  server.handle("GET", "/api/v1/file", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    const auto path_q = query_get(req.query, "path");
    const auto yolo_q = query_get(req.query, "yolo");
    const bool yolo = yolo_q && (*yolo_q == "1" || *yolo_q == "true");
    if (!path_q || path_q->empty()) {
      resp->status = 400;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"missing path"})";
      return;
    }

    const std::filesystem::path user_path(*path_q);
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path scope_root = std::filesystem::path(cfg.host_scope_root);

    std::filesystem::path resolved;
    if (yolo) {
      resolved = user_path.is_absolute() ? user_path : (cwd / user_path);
    } else {
      if (user_path.is_absolute()) {
        resp->status = 400;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"absolute path requires yolo=1"})";
        return;
      }
      resolved = (scope_root / user_path);
    }
    resolved = resolved.lexically_normal();

    // Containment check when not yolo.
    if (!yolo) {
      std::error_code ec;
      const auto canon_root = std::filesystem::weakly_canonical(scope_root, ec);
      if (ec) {
        resp->status = 500;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"failed to canonicalize host scope root"})";
        return;
      }
      ec.clear();
      const auto canon_file = std::filesystem::weakly_canonical(resolved, ec);
      if (ec) {
        resp->status = 404;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"file not found"})";
        return;
      }
      const auto root_s = canon_root.native();
      const auto file_s = canon_file.native();
      if (file_s.size() < root_s.size() || file_s.compare(0, root_s.size(), root_s) != 0) {
        resp->status = 403;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"path escapes host scope"})";
        return;
      }
    }

    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) || !std::filesystem::is_regular_file(resolved, ec)) {
      resp->status = 404;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"file not found"})";
      return;
    }

    const uintmax_t max_bytes = 10ULL * 1024ULL * 1024ULL;
    const uintmax_t sz = std::filesystem::file_size(resolved, ec);
    if (ec || sz > max_bytes) {
      resp->status = 400;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"file too large"})";
      return;
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in.is_open()) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to open file"})";
      return;
    }
    std::string bytes;
    bytes.resize((size_t)sz);
    in.read(bytes.data(), (std::streamsize)bytes.size());
    if (!in) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to read file"})";
      return;
    }

    resp->status = 200;
    resp->headers["Content-Type"] = content_type_from_path(resolved);
    resp->body = std::move(bytes);
  });

  server.handle("POST", "/api/v1/run", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";

#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    Json::Value args;
    std::string perr;
    if (!json_parse_object(req.body, &args, &perr)) {
      resp->status = 400;
      resp->body = json_stringify(Json::Value(Json::objectValue));
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string("invalid JSON: ") + perr;
      resp->body = json_stringify(o);
      return;
    }

    const std::string prompt = args.isMember("prompt") && args["prompt"].isString() ? args["prompt"].asString() : "";
    if (prompt.empty()) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "missing prompt";
      resp->body = json_stringify(o);
      return;
    }

    OpenAIClientConfig run_cfg = ocfg;
    if (args.isMember("model") && args["model"].isString()) run_cfg.model = args["model"].asString();
    if (args.isMember("base_url") && args["base_url"].isString()) run_cfg.base_url = args["base_url"].asString();
    if (args.isMember("api_key") && args["api_key"].isString()) run_cfg.api_key = args["api_key"].asString();

    const std::string tools = args.isMember("tools") && args["tools"].isString() ? args["tools"].asString() : cfg.tools;
    const bool yolo = args.isMember("yolo") && args["yolo"].isBool() ? args["yolo"].asBool() : cfg.yolo_default;
    std::string tools_root = args.isMember("tools_root") && args["tools_root"].isString() ? args["tools_root"].asString() : cfg.tools_root;
    if (tools_root == "@host") {
      tools_root = cfg.host_scope_root;
    } else if (tools_root == "@cwd") {
      tools_root = "";
    }
    if (yolo) {
      // YOLO means "no restriction" for host tools. In practice this currently controls
      // file edit scoping (`file_apply_patch` root + unsafe paths).
      tools_root.clear();
    }
    const size_t max_steps = args.isMember("max_steps") && args["max_steps"].isUInt64() ? (size_t)args["max_steps"].asUInt64() : 0;
    const bool trace = !(args.isMember("trace") && args["trace"].isBool() && args["trace"].asBool() == false);
    const bool verbose = args.isMember("verbose") && args["verbose"].isBool() ? args["verbose"].asBool() : false;

    const std::string session_id = args.isMember("session_id") && args["session_id"].isString() ? args["session_id"].asString() : "default";
    const bool no_session = args.isMember("no_session") && args["no_session"].isBool() ? args["no_session"].asBool() : false;

    agent_session_t* session = nullptr;
    SessionStoreConfig store_cfg;
    store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();
    if (!no_session) {
      const agent_status_t st = session_store_load(store_cfg, session_id, &session);
      if (st != AGENT_OK) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to load session";
        o["status"] = (int)st;
        resp->body = json_stringify(o);
        return;
      }
    } else {
      const agent_status_t st = agent_session_create(&session);
      if (st != AGENT_OK) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to create session";
        o["status"] = (int)st;
        resp->body = json_stringify(o);
        return;
      }
    }

    agent_tool_registry_t* registry = nullptr;
    agent_tool_executor_t executor{};
    bool need_destroy_executor = false;
    const bool use_tool_loop = (tools != "none");

    if (tools == "basic") {
      if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to init toolset_basic";
        resp->body = json_stringify(o);
        agent_session_destroy(session);
        return;
      }
    } else if (tools == "host") {
      HostToolsetConfig hcfg;
      hcfg.root_dir = tools_root;
      if (toolset_host_create(hcfg, &registry, &executor) != AGENT_OK) {
        resp->status = 500;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to init toolset_host";
        resp->body = json_stringify(o);
        agent_session_destroy(session);
        return;
      }
      need_destroy_executor = true;
    } else if (tools != "none") {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid tools (expected: none|basic|host)";
      resp->body = json_stringify(o);
      agent_session_destroy(session);
      return;
    }

    bool ok = false;
    std::string assistant_text;
    std::string err;
    long http_status = 0;
    std::string http_body;
    std::ostringstream trace_buf;
    std::ostream* trace_stream = trace ? &trace_buf : nullptr;
    Json::Value events_out; // optional, populated when tool loop captures structured events

    if (use_tool_loop) {
      ToolLoopOptions opt;
      opt.max_steps = max_steps;
      opt.verbose = verbose;
      if (args.isMember("force_tool") && args["force_tool"].isString()) opt.force_tool = args["force_tool"].asString();
      opt.require_tool_call = args.isMember("require_tool_call") && args["require_tool_call"].isBool() ? args["require_tool_call"].asBool() : false;

      ToolLoopResult r;
      ok = run_tool_loop(run_cfg, session, prompt, registry, &executor, opt, trace_stream, &r, &err, &http_status, &http_body);
      assistant_text = r.final_assistant_text;
      if (!r.events_json.empty()) {
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream iss(r.events_json);
        Json::Value ev;
        if (Json::parseFromStream(rb, iss, &ev, &errs) && ev.isArray()) {
          events_out = ev;
        }
      }
      if (ok) {
        agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());
        agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
      }
    } else {
      agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());

      ProviderCtx pctx;
      pctx.cfg = run_cfg;
      agent_provider_t provider;
      provider.ctx = &pctx;
      provider.generate = provider_generate;

      agent_run_options_t run_opt{};
      run_opt.model = run_cfg.model.c_str();
      run_opt.max_chars = 0; // daemon does not enforce compaction yet
      run_opt.keep_last_messages = 0;
      run_opt.summary_or_null = nullptr;

      agent_run_report_t rep{};
      const agent_status_t st = agent_run_once(session, &provider, &run_opt, &rep);
      ok = (st == AGENT_OK);
      assistant_text = ok ? std::string(rep.assistant_view.content, rep.assistant_view.content_len) : "";
      if (!ok) {
        err = pctx.last_error.empty() ? "agent_run_once failed" : pctx.last_error;
        http_status = pctx.last_http_status;
        http_body = pctx.last_body;
      } else {
        agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
      }
      if (trace_stream) {
        *trace_stream << "=== REQUEST ===\n";
        *trace_stream << (pctx.last_request_body.empty() ? "(request body unavailable)\n" : (pctx.last_request_body + "\n"));
        *trace_stream << "=== RESPONSE ===\n";
        *trace_stream << (pctx.last_body.empty() ? "" : (pctx.last_body + "\n"));
      }
    }

    if (ok && !no_session) {
      (void)session_store_save(store_cfg, session_id, session);
    }

    if (registry) {
      agent_tool_registry_destroy(registry);
    }
    if (need_destroy_executor) {
      toolset_host_destroy(&executor);
    }
    agent_session_destroy(session);

    Json::Value out(Json::objectValue);
    out["ok"] = ok;
    out["assistant_text"] = assistant_text;
    if (!ok && !err.empty()) out["error"] = err;
    out["http_status"] = (Json::Int64)http_status;
    out["http_body"] = http_body;
    out["trace_text"] = trace_buf.str();
    out["effective_tools_root"] = tools_root;
    out["effective_yolo"] = yolo;
    out["verbose"] = verbose;
    // Structured event log for UIs (LLM requests/responses + tool calls/results).
    if (use_tool_loop && events_out.isArray()) {
      out["events"] = events_out;
    }
    resp->body = json_stringify(out);
#endif
  });

  std::string err;
  std::cerr << "agentd listening on http://" << cfg.listen_host << ":" << cfg.listen_port << "\n";
  if (!server.serve(cfg.listen_host, cfg.listen_port, &err)) {
    std::cerr << "agentd failed: " << err << "\n";
    return 1;
  }
  return 0;
}
