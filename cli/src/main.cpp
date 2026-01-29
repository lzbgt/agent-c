#include "agent/agent.h"
#include "agent/runner.h"

#include "openai_client.h"
#include "session_store.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <cctype>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string lower_copy(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static bool url_contains_ci(const std::string& url, const std::string& needle) {
  if (needle.empty()) return false;
  const std::string u = lower_copy(url);
  const std::string n = lower_copy(needle);
  return u.find(n) != std::string::npos;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  // Fallback to current directory.
  return std::filesystem::current_path().string();
}

static const char* default_host_system_prompt() {
  // This prompt is host-only policy (CLI/daemon), not core behavior.
  //
  // Goal: push the model toward fast, incremental inspection instead of reading huge files,
  // and toward auditable file edits.
  return
    "You are a host-side coding agent with access to system tools (shell/proc exec), bounded filesystem read tools, and a diff-based file edit tool.\n"
    "\n"
    "Efficiency rules (important):\n"
    "- Prefer bounded/paginated inspection over reading full files.\n"
    "  - Use fs_list/fs_stat to inspect directories/files with predictable output size.\n"
    "    - Note: fs_list excludes common huge dirs (node_modules/build/dist) by default; disable with use_default_excludes=false.\n"
    "  - Use fs_read with start_line/max_lines (and optional end_line) for paging through files.\n"
    "  - Use rg/grep/head/tail/sed/awk for narrow, targeted inspection when appropriate.\n"
    "- Avoid dumping large directories or entire files unless strictly needed.\n"
    "- When exploring code, start narrow (file list, search hits) then open only the relevant sections.\n"
    "\n"
    "Edits:\n"
    "- Use the diff-based edit tool for changing files so edits are auditable.\n"
    "- For one-off inspection, prefer read-only commands.\n"
    "\n"
    "Tool outputs:\n"
    "- Tool success is not just exit code; judge using tool output content.\n";
}

static void usage() {
  std::cerr
    << "Usage:\n"
    << "  agent run \"prompt text\" [options]\n"
    << "  agent chat [options]\n\n"
    << "Options:\n"
    << "  --model <name>            Model name (default: gpt-4o-mini)\n"
    << "  --base-url <url>          API base url (default: https://api.openai.com/v1)\n"
    << "  --api-key <key>           API key (default: OPENAI_API_KEY)\n"
    << "  --proxy <url>             Optional HTTP proxy override (else env HTTPS_PROXY/http_proxy)\n"
    << "  --timeout-ms <n>          HTTP timeout in ms (default: 60000)\n"
    << "  --trace                   Print full request/response/tool transcript to stderr (default: on)\n"
    << "  --quiet                   Suppress transcript; print assistant text only\n"
    << "  --session <id>            Session id to load/save (default: default)\n"
    << "  --no-session              Disable persistence (ephemeral run)\n"
    << "  --system <text>           Add a system message at the start (one time)\n"
    << "  --no-default-system       Disable the default host system hint (host tools only)\n"
    << "  --max-chars <n>           Auto-compact when session exceeds n chars (default: 20000)\n"
    << "  --keep-last <n>           Keep last n messages during compaction (default: 16)\n"
    << "  --tools none|basic|host   Select toolset (default: host)\n"
    << "  --tools-root <path>       Root/working dir for host file edits (file_apply_patch) (default: current dir)\n"
    << "  --force-tool <name>       Force a tool call on first step (verification)\n"
    << "  --require-tool-call       Fail if no tool call occurred\n"
    << "  --max-steps <n>           Max tool loop steps (default: unlimited; 0 means unlimited)\n";
}

static bool take_switch(std::vector<std::string>& args, const std::string& flag, bool* out_enabled) {
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == flag) {
      *out_enabled = true;
      args.erase(args.begin() + (long)i);
      return true;
    }
  }
  return true;
}

static bool take_flag(std::vector<std::string>& args, const std::string& flag, std::string* out_value) {
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == flag) {
      if (i + 1 >= args.size()) {
        return false;
      }
      *out_value = args[i + 1];
      args.erase(args.begin() + (long)i, args.begin() + (long)i + 2);
      return true;
    }
  }
  return true;
}

static bool take_flag_u64(std::vector<std::string>& args, const std::string& flag, size_t* out_value) {
  std::string v;
  if (!take_flag(args, flag, &v)) {
    return false;
  }
  if (v.empty()) {
    return true;
  }
  try {
    *out_value = static_cast<size_t>(std::stoull(v));
  } catch (...) {
    return false;
  }
  return true;
}

static bool take_enum(std::vector<std::string>& args, const std::string& flag, std::string* out_value) {
  std::string v;
  if (!take_flag(args, flag, &v)) {
    return false;
  }
  if (!v.empty()) {
    *out_value = v;
  }
  return true;
}

struct CurlProviderCtx {
  OpenAIClientConfig cfg;
  long last_http_status = 0;
  std::string last_body;
  std::string last_request_body;
  std::string last_error;
};

static agent_status_t curl_provider_generate(
  void* provider_ctx,
  const agent_generate_request_t* req,
  agent_generate_response_t* out_resp
) {
  if (!provider_ctx || !req || !out_resp) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  auto* ctx = static_cast<CurlProviderCtx*>(provider_ctx);
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
    // If parsing failed, treat as error (caller can inspect last_body).
    if (ctx->last_error.empty()) {
      ctx->last_error = "failed to extract assistant text from response";
    }
    return AGENT_ERR_INTERNAL;
  }
  return agent_string_set_copy(&out_resp->assistant_text, r.assistant_text.c_str(), r.assistant_text.size());
}

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve((size_t)argc);
  for (int i = 1; i < argc; i++) {
    args.emplace_back(argv[i]);
  }

  if (args.empty()) {
    usage();
    return 2;
  }

  const std::string cmd = args[0];
  args.erase(args.begin());
  if (cmd != "run" && cmd != "chat") {
    usage();
    return 2;
  }

  std::string model = "gpt-4o-mini";
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string proxy_url;
  std::string session_id = "default";
  bool no_session = false;
  std::string system_msg;
  bool no_default_system = false;
  size_t max_chars = 20000;
  size_t keep_last = 16;
  size_t timeout_ms = 60000;
  std::string tools_mode = "host";
  std::string tools_root; // empty => unrestricted (YOLO)
  std::string force_tool;
  bool require_tool_call = false;
  size_t max_steps = 0; // unlimited unless explicitly set
  bool trace = true;
  bool quiet = false;

  if (!take_flag(args, "--model", &model)) {
    std::cerr << "Missing value for --model\n";
    return 2;
  }
  if (!take_flag(args, "--base-url", &base_url)) {
    std::cerr << "Missing value for --base-url\n";
    return 2;
  }
  if (!take_flag(args, "--api-key", &api_key)) {
    std::cerr << "Missing value for --api-key\n";
    return 2;
  }
  if (!take_flag(args, "--proxy", &proxy_url)) {
    std::cerr << "Missing value for --proxy\n";
    return 2;
  }
  if (!take_flag_u64(args, "--timeout-ms", &timeout_ms)) {
    std::cerr << "Invalid value for --timeout-ms\n";
    return 2;
  }
  if (!take_switch(args, "--trace", &trace)) {
    std::cerr << "Invalid flag: --trace\n";
    return 2;
  }
  if (!take_switch(args, "--quiet", &quiet)) {
    std::cerr << "Invalid flag: --quiet\n";
    return 2;
  }
  if (quiet) {
    trace = false;
  }
  if (!take_switch(args, "--no-session", &no_session)) {
    std::cerr << "Invalid flag: --no-session\n";
    return 2;
  }
  if (!take_flag(args, "--session", &session_id)) {
    std::cerr << "Missing value for --session\n";
    return 2;
  }
  if (!take_flag(args, "--system", &system_msg)) {
    std::cerr << "Missing value for --system\n";
    return 2;
  }
  if (!take_switch(args, "--no-default-system", &no_default_system)) {
    std::cerr << "Invalid flag: --no-default-system\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-chars", &max_chars)) {
    std::cerr << "Invalid value for --max-chars\n";
    return 2;
  }
  if (!take_flag_u64(args, "--keep-last", &keep_last)) {
    std::cerr << "Invalid value for --keep-last\n";
    return 2;
  }
  if (!take_enum(args, "--tools", &tools_mode)) {
    std::cerr << "Missing value for --tools\n";
    return 2;
  }
  if (!take_flag(args, "--tools-root", &tools_root)) {
    std::cerr << "Missing value for --tools-root\n";
    return 2;
  }
  if (!take_flag(args, "--force-tool", &force_tool)) {
    std::cerr << "Missing value for --force-tool\n";
    return 2;
  }
  if (!take_switch(args, "--require-tool-call", &require_tool_call)) {
    std::cerr << "Invalid flag: --require-tool-call\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-steps", &max_steps)) {
    std::cerr << "Invalid value for --max-steps\n";
    return 2;
  }

  std::string prompt;
  if (cmd == "run") {
    // Remaining args should be the prompt (single token or quoted string as one arg).
    if (args.empty()) {
      std::cerr << "Missing prompt\n";
      return 2;
    }
    prompt = args[0];
  }

  // Host-side env defaults (core remains env-free).
  if (base_url == "https://api.openai.com/v1") {
    if (const char* b = getenv_s("OPENAI_API_BASE")) {
      base_url = b;
    } else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) {
      base_url = b2;
    } else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) {
      base_url = b3;
    } else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) {
      base_url = b4;
    }
  }
  // Pick the API key that matches the chosen base URL. This prevents accidental mix-ups when the
  // host environment exports multiple provider keys.
  if (api_key.empty()) {
    if (url_contains_ci(base_url, "deepseek")) {
      if (const char* k = getenv_s("DEEPSEEK_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) api_key = k3;
    } else if (url_contains_ci(base_url, "openrouter")) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) api_key = k3;
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) api_key = k3;
    }
  }
  if (api_key.empty()) {
    std::cerr << "Missing API key. Provide --api-key or set OPENAI_API_KEY / OPENROUTER_API_KEY / DEEPSEEK_API_KEY.\n";
    return 2;
  }

  // Session load (mandatory for CLI/daemon use cases, but optional via flag).
  agent_session_t* session = nullptr;
  SessionStoreConfig store_cfg;
  store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();

  if (!no_session && !session_id.empty()) {
    const agent_status_t st = session_store_load(store_cfg, session_id, &session);
    if (st != AGENT_OK) {
      std::cerr << "Failed to load session: " << (int)st << "\n";
      return 1;
    }
  } else {
    const agent_status_t st = agent_session_create(&session);
    if (st != AGENT_OK) {
      std::cerr << "Failed to create session: " << (int)st << "\n";
      return 1;
    }
  }

  // Add one-time system message if requested and session is empty.
  if (!system_msg.empty() && agent_session_message_count(session) == 0) {
    agent_session_add_message(session, AGENT_ROLE_SYSTEM, system_msg.c_str());
  }

  CurlProviderCtx pctx;
  pctx.cfg.base_url = base_url;
  pctx.cfg.api_key = api_key;
  pctx.cfg.model = model;
  pctx.cfg.proxy_url = proxy_url;
  pctx.cfg.timeout_ms = (long)timeout_ms;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    pctx.cfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    pctx.cfg.openrouter_x_title = t;
  }

  if (tools_mode == "yolo") {
    tools_mode = "host";
  }

  // Add host-only default system message (one-time) when using host tools and the session is empty.
  // This encourages incremental inspection (rg/head/awk) instead of full file dumps.
  if (!no_default_system && system_msg.empty() && tools_mode == "host" && agent_session_message_count(session) == 0) {
    agent_session_add_message(session, AGENT_ROLE_SYSTEM, default_host_system_prompt());
  }

  if (tools_mode != "none") {
    agent_tool_registry_t* registry = nullptr;
    agent_tool_executor_t executor{};
    bool need_destroy_executor = false;
    if (tools_mode == "basic") {
      if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
        std::cerr << "Failed to initialize toolset\n";
        agent_session_destroy(session);
        return 1;
      }
    } else if (tools_mode == "host") {
      HostToolsetConfig cfg;
      cfg.root_dir = tools_root;
      if (toolset_host_create(cfg, &registry, &executor) != AGENT_OK) {
        std::cerr << "Failed to initialize host toolset\n";
        agent_session_destroy(session);
        return 1;
      }
      need_destroy_executor = true;
    } else {
      std::cerr << "Unsupported --tools mode: " << tools_mode << "\n";
      agent_session_destroy(session);
      return 2;
    }

    ToolLoopOptions opt;
    opt.force_tool = force_tool;
    opt.require_tool_call = require_tool_call;
    opt.max_steps = max_steps;
    opt.max_chars = max_chars;
    opt.keep_last_messages = keep_last;

    auto run_one = [&](const std::string& user_prompt) -> int {
      ToolLoopResult r;
      std::string err;
      long http_status = 0;
      std::string http_body;
      std::ostream* trace_stream = trace ? &std::cerr : nullptr;
      if (!run_tool_loop(pctx.cfg, session, user_prompt, registry, &executor, opt, trace_stream, &r, &err, &http_status, &http_body)) {
        if (http_status) {
          std::cerr << openai_format_http_error(http_status, http_body) << "\n";
          if (!http_body.empty()) {
            std::cerr << http_body << "\n";
          }
        } else if (!err.empty()) {
          std::cerr << "Tool loop failed: " << err << "\n";
        } else {
          std::cerr << "Tool loop failed\n";
        }
        return 1;
      }

      // Persist a portable transcript into the session:
      // - user prompt
      // - tool calls + tool results as assistant text markers (do not store raw "tool" role messages,
      //   because OpenAI-compatible APIs often require tool_call_id fields we don't store in core sessions).
      // - final assistant message
      agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());
      for (const auto& rec : r.tool_records) {
        std::string call = "[tool_call] name=" + rec.tool_name;
        if (!rec.tool_call_id.empty()) call += " id=" + rec.tool_call_id;
        call += "\n" + rec.arguments_json;
        agent_session_add_message(session, AGENT_ROLE_ASSISTANT, call.c_str());

        std::string out = "[tool_result] name=" + rec.tool_name;
        if (!rec.tool_call_id.empty()) out += " id=" + rec.tool_call_id;
        out += "\n" + (rec.result_string_for_prompt.empty() ? rec.result_string : rec.result_string_for_prompt);
        agent_session_add_message(session, AGENT_ROLE_ASSISTANT, out.c_str());
      }
      agent_session_add_message(session, AGENT_ROLE_ASSISTANT, r.final_assistant_text.c_str());
      std::cout << r.final_assistant_text << "\n";
      return 0;
    };

    int rc = 0;
    if (cmd == "run") {
      rc = run_one(prompt);
    } else {
      std::string line;
      while (true) {
        std::cerr << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
          break;
        }
        if (line == "/exit" || line == "/quit") {
          break;
        }
        if (line.empty()) {
          continue;
        }
        rc = run_one(line);
        if (rc != 0) {
          break;
        }
      }
    }

    if (!no_session && !session_id.empty()) {
      const agent_status_t st = session_store_save(store_cfg, session_id, session);
      if (st != AGENT_OK) {
        std::cerr << "Failed to save session: " << (int)st << "\n";
        rc = 1;
      }
    }

    agent_tool_registry_destroy(registry);
    if (need_destroy_executor) {
      toolset_host_destroy(&executor);
    }
    agent_session_destroy(session);
    return rc;
  } else {
    auto run_one = [&](const std::string& user_prompt) -> int {
      agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());

      agent_provider_t provider;
      provider.ctx = &pctx;
      provider.generate = curl_provider_generate;

      agent_run_options_t run_opt;
      run_opt.model = model.c_str();
      run_opt.keep_last_messages = keep_last;
      run_opt.summary_or_null = nullptr;

      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
      for (int attempt = 0; attempt < 3; attempt++) {
        run_opt.max_chars = attempt_max_chars;

        agent_run_report_t rep{};
        const agent_status_t st = agent_run_once(session, &provider, &run_opt, &rep);
        if (st == AGENT_OK) {
          if (trace) {
            std::cerr << "=== REQUEST (attempt=" << attempt << ") ===\n";
            if (!pctx.last_request_body.empty()) {
              std::cerr << pctx.last_request_body << "\n";
            } else {
              std::cerr << "(request body unavailable)\n";
            }
            std::cerr << "=== RESPONSE ===\n";
            if (!pctx.last_body.empty()) {
              std::cerr << pctx.last_body << "\n";
            }
            std::cerr << "=== COMPACTION ===\n";
            std::cerr << "before_chars=" << rep.compact.before_chars
                      << " after_chars=" << rep.compact.after_chars
                      << " dropped=" << rep.compact.dropped_messages
                      << " inserted_summary=" << (int)rep.compact.inserted_summary
                      << "\n";
          }
          std::cout << std::string(rep.assistant_view.content, rep.assistant_view.content_len) << "\n";
          return 0;
        }

        // Retry on context-too-long rejections by compacting more aggressively (session rotation).
        if (attempt < 2 && openai_is_context_too_long_error(pctx.last_http_status, pctx.last_body)) {
          const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
          if (trace) {
            std::cerr << "=== RETRY (context too long) ===\n";
            std::cerr << "attempt=" << attempt << " http_status=" << pctx.last_http_status
                      << " max_chars_before=" << attempt_max_chars
                      << " max_chars_after=" << next << "\n";
          }
          attempt_max_chars = next;
          continue;
        }

        if (pctx.last_http_status) {
          if (!pctx.last_error.empty()) {
            std::cerr << pctx.last_error << "\n";
          } else {
            std::cerr << openai_format_http_error(pctx.last_http_status, pctx.last_body) << "\n";
          }
          if (!pctx.last_body.empty()) {
            std::cerr << pctx.last_body << "\n";
          }
        } else {
          std::cerr << "agent_run_once failed: " << (int)st << "\n";
        }
        return 1;
      }

      std::cerr << "agent_run_once failed after retries\n";
      return 1;
    };

    int rc = 0;
    if (cmd == "run") {
      rc = run_one(prompt);
    } else {
      std::string line;
      while (true) {
        std::cerr << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
          break;
        }
        if (line == "/exit" || line == "/quit") {
          break;
        }
        if (line.empty()) {
          continue;
        }
        rc = run_one(line);
        if (rc != 0) {
          break;
        }
      }
    }

    if (!no_session && !session_id.empty()) {
      const agent_status_t st = session_store_save(store_cfg, session_id, session);
      if (st != AGENT_OK) {
        std::cerr << "Failed to save session: " << (int)st << "\n";
        rc = 1;
      }
    }

    agent_session_destroy(session);
    return rc;
  }

  // Unreachable.
}
