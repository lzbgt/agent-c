#include "host_session_prompts.h"

#include "agent/agent.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void write_text(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  assert(out.good());
  out << content;
  assert(out.good());
}

static std::string message_text(const agent_session_t* session, size_t index) {
  agent_message_view_t v{};
  assert(agent_session_get_message(session, index, &v) == AGENT_OK);
  return std::string(v.content ? v.content : "", v.content_len);
}

int main() {
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path temp_root = fs::temp_directory_path() / ("agent_host_session_prompts_" + unique);
  const fs::path nested = temp_root / "repo" / "pkg";
  fs::create_directories(nested);

  write_text(temp_root / "repo" / "AGENTS.md", "- root rule: always verify\n");
  write_text(nested / "AGENTS.md", "- nested rule: prefer narrow diffs\n");

  const std::string project_prompt = build_project_instructions_system_prompt(nested);
  assert(project_prompt.rfind(kProjectInstructionsPrefix, 0) == 0);
  assert(project_prompt.find("[AGENTS.md]") != std::string::npos);
  assert(project_prompt.find("[pkg/AGENTS.md]") != std::string::npos);
  assert(project_prompt.find("root rule: always verify") < project_prompt.find("nested rule: prefer narrow diffs"));

  agent_session_t* session = nullptr;
  assert(agent_session_create(&session) == AGENT_OK);
  assert(agent_session_add_message(session, AGENT_ROLE_USER, "hello") == AGENT_OK);
  const bool changed = ensure_pinned_host_session_prompts(
    &session,
    "default",
    "CLIENT_PROFILE=webui\n\n- durable scene only\n",
    "CLIENT_PROFILE=webui",
    project_prompt
  );
  assert(changed);
  assert(agent_session_message_count(session) == 4);
  assert(message_text(session, 0).find("You are a host-side coding agent") == 0);
  assert(message_text(session, 0).find("HOST_SYSTEM_PROFILE=default") != std::string::npos);
  assert(message_text(session, 1).find("CLIENT_PROFILE=webui") == 0);
  assert(message_text(session, 2) == project_prompt);
  assert(message_text(session, 3) == "hello");
  assert(!ensure_pinned_host_session_prompts(
    &session,
    "default",
    "CLIENT_PROFILE=webui\n\n- durable scene only\n",
    "CLIENT_PROFILE=webui",
    project_prompt
  ));
  agent_session_destroy(session);

  agent_session_t* stale = nullptr;
  assert(agent_session_create(&stale) == AGENT_OK);
  assert(agent_session_add_message(stale, AGENT_ROLE_SYSTEM, "You are a host-side coding agent\nHOST_SYSTEM_PROFILE=default\n") == AGENT_OK);
  assert(agent_session_add_message(stale, AGENT_ROLE_SYSTEM, "PROJECT_INSTRUCTIONS=agmd-v1\n\n[AGENTS.md]\n- stale rule\n") == AGENT_OK);
  assert(agent_session_add_message(stale, AGENT_ROLE_USER, "keep me") == AGENT_OK);
  assert(ensure_pinned_host_session_prompts(&stale, "default", "", "", project_prompt));
  assert(agent_session_message_count(stale) == 3);
  assert(message_text(stale, 1) == project_prompt);
  assert(message_text(stale, 2) == "keep me");
  agent_session_destroy(stale);

  fs::remove_all(temp_root);
  return 0;
}
