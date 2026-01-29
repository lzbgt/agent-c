#include "agent/agent.h"

#include "openai_client.h"
#include "session_store.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  // Fallback to current directory.
  return std::filesystem::current_path().string();
}

static void usage() {
  std::cerr
    << "Usage:\n"
    << "  agent run \"prompt text\" [options]\n\n"
    << "Options:\n"
    << "  --model <name>            Model name (default: gpt-4o-mini)\n"
    << "  --base-url <url>          API base url (default: https://api.openai.com/v1)\n"
    << "  --api-key <key>           API key (default: OPENAI_API_KEY)\n"
    << "  --session <id>            Session id to load/save (default: default)\n"
    << "  --no-session              Disable persistence (ephemeral run)\n"
    << "  --system <text>           Add a system message at the start (one time)\n"
    << "  --max-chars <n>           Auto-compact when session exceeds n chars (default: 20000)\n"
    << "  --keep-last <n>           Keep last n messages during compaction (default: 16)\n";
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
  if (cmd != "run") {
    usage();
    return 2;
  }
  if (args.empty()) {
    usage();
    return 2;
  }

  std::string model = "gpt-4o-mini";
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string session_id = "default";
  bool no_session = false;
  std::string system_msg;
  size_t max_chars = 20000;
  size_t keep_last = 16;

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
  if (!take_flag_u64(args, "--max-chars", &max_chars)) {
    std::cerr << "Invalid value for --max-chars\n";
    return 2;
  }
  if (!take_flag_u64(args, "--keep-last", &keep_last)) {
    std::cerr << "Invalid value for --keep-last\n";
    return 2;
  }

  // Remaining args should be the prompt (single token or quoted string as one arg).
  if (args.empty()) {
    std::cerr << "Missing prompt\n";
    return 2;
  }
  const std::string prompt = args[0];

  // Host-side env defaults (core remains env-free).
  if (api_key.empty()) {
    if (const char* k = getenv_s("OPENAI_API_KEY")) {
      api_key = k;
    } else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) {
      api_key = k2;
    } else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) {
      api_key = k3;
    }
  }
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
  if (api_key.empty()) {
    std::cerr << "Missing API key. Provide --api-key or set OPENAI_API_KEY / OPENROUTER_API_KEY.\n";
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

  agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());

  agent_compact_report_t compact{};
  agent_session_compact_char_budget(session, max_chars, keep_last, nullptr, &compact);

  OpenAIClientConfig client_cfg;
  client_cfg.base_url = base_url;
  client_cfg.api_key = api_key;
  client_cfg.model = model;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    client_cfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    client_cfg.openrouter_x_title = t;
  }

  const OpenAIChatResult r = openai_chat_completions(client_cfg, session);
  if (r.http_status < 200 || r.http_status >= 300) {
    std::cerr << "HTTP " << r.http_status << "\n" << r.response_body << "\n";
    agent_session_destroy(session);
    return 1;
  }

  const std::string assistant = r.assistant_text.empty() ? r.response_body : r.assistant_text;
  std::cout << assistant << "\n";

  agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant.c_str());

  if (!no_session && !session_id.empty()) {
    const agent_status_t st = session_store_save(store_cfg, session_id, session);
    if (st != AGENT_OK) {
      std::cerr << "Failed to save session: " << (int)st << "\n";
      agent_session_destroy(session);
      return 1;
    }
  }

  agent_session_destroy(session);
  return 0;
}
