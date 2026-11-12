#include "host_session_prompts.h"

#include "agent/agent.h"
#include "agent/parts.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
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

  // UTC and the host's local day can differ, including across a year boundary.
  const char* old_tz_value = std::getenv("TZ");
  const bool had_tz = old_tz_value != nullptr;
  const std::string old_tz = old_tz_value ? old_tz_value : "";
  setenv("TZ", "CST-8", 1);
  tzset();
  constexpr std::time_t before_local_new_year = 1798732799; // 2026-12-31 15:59:59 UTC.
  constexpr std::time_t local_new_year = 1798732800;
  constexpr std::time_t utc_new_year = 1798761600;
  const auto before_date = build_current_date_system_prompt(before_local_new_year);
  const auto after_date = build_current_date_system_prompt(local_new_year);
  assert(before_date.find("Host local date:") < before_date.find("Current UTC date:"));
  assert(before_date.find("Current UTC date: 2026-12-31.") != std::string::npos);
  assert(before_date.find("Host local date: 2026-12-31 (UTC offset +0800") != std::string::npos);
  assert(after_date.find("Current UTC date: 2026-12-31.") != std::string::npos);
  assert(after_date.find("Host local date: 2027-01-01 (UTC offset +0800") != std::string::npos);
  assert(before_date == build_current_date_system_prompt(before_local_new_year - 1));

  agent_session_t* dated = nullptr;
  assert(agent_session_create(&dated) == AGENT_OK);
  assert(ensure_current_date_system_prompt(&dated, before_local_new_year));
  assert(agent_session_message_count(dated) == 1);
  assert(message_text(dated, 0) == before_date);
  assert(!ensure_current_date_system_prompt(&dated, before_local_new_year - 1));
  assert(agent_session_add_message(dated, AGENT_ROLE_USER, "Historical 2025 request must remain intact") == AGENT_OK);
  assert(agent_session_add_message(dated, AGENT_ROLE_ASSISTANT, "Retain prior answer") == AGENT_OK);
  assert(agent_session_add_message(dated, AGENT_ROLE_SYSTEM, "CURRENT_DATE_CONTEXT=v1\nStale imported clock") == AGENT_OK);
  assert(ensure_current_date_system_prompt(&dated, local_new_year));
  assert(agent_session_message_count(dated) == 3);
  assert(message_text(dated, 0) == after_date);
  assert(message_text(dated, 1) == "Historical 2025 request must remain intact");
  assert(message_text(dated, 2) == "Retain prior answer");
  assert(!ensure_current_date_system_prompt(&dated, local_new_year + 60));
  assert(ensure_current_date_system_prompt(&dated, utc_new_year));
  assert(message_text(dated, 0).find("Current UTC date: 2027-01-01.") != std::string::npos);
  assert(agent_session_message_count(dated) == 3);
  agent_session_destroy(dated);

  agent_session_t* multimodal = nullptr;
  assert(agent_session_create(&multimodal) == AGENT_OK);
  assert(agent_session_add_message(multimodal, AGENT_ROLE_SYSTEM, "Owner instructions") == AGENT_OK);
  const uint8_t binary[] = {0, 1, 255};
  agent_content_part_t parts[3]{};
  parts[0].type = AGENT_PART_TEXT;
  parts[0].text_or_null = "Preserve attachments";
  parts[1].type = AGENT_PART_IMAGE_URL;
  parts[1].url_or_null = "https://example.org/image.png";
  parts[2].type = AGENT_PART_BINARY;
  parts[2].bytes_or_null = binary;
  parts[2].bytes_len = sizeof(binary);
  parts[2].mime_or_null = "application/octet-stream";
  assert(agent_session_add_message_parts(multimodal, AGENT_ROLE_USER, parts, 3) == AGENT_OK);
  const auto check_parts = [&](size_t index) {
    assert(message_text(multimodal, index) == "Preserve attachments");
    assert(agent_session_message_part_count(multimodal, index) == 3);
    agent_content_part_view_t view{};
    assert(agent_session_get_message_part(multimodal, index, 1, &view) == AGENT_OK);
    assert(view.type == AGENT_PART_IMAGE_URL && std::string(view.url, view.url_len) == "https://example.org/image.png");
    assert(agent_session_get_message_part(multimodal, index, 2, &view) == AGENT_OK);
    assert(view.type == AGENT_PART_BINARY && view.bytes_len == sizeof(binary));
    assert(view.bytes[0] == 0 && view.bytes[1] == 1 && view.bytes[2] == 255);
    assert(std::string(view.mime, view.mime_len) == "application/octet-stream");
  };
  assert(ensure_current_date_system_prompt(&multimodal, before_local_new_year));
  check_parts(2);
  assert(ensure_current_date_system_prompt(&multimodal, local_new_year));
  check_parts(2);
  assert(ensure_pinned_host_session_prompts(&multimodal, "default", "", "", ""));
  check_parts(3);
  const std::string original_client = "CLIENT_PROFILE=webui\nOriginal output instructions";
  const std::string revised_client = "CLIENT_PROFILE=webui\nUpdated output instructions";
  assert(ensure_pinned_host_session_prompts(&multimodal, "default", original_client, "CLIENT_PROFILE=webui", ""));
  check_parts(4);
  assert(ensure_pinned_host_session_prompts(&multimodal, "default", revised_client, "CLIENT_PROFILE=webui", ""));
  assert(message_text(multimodal, 1) == revised_client);
  check_parts(4);
  assert(!ensure_pinned_host_session_prompts(&multimodal, "default", revised_client, "CLIENT_PROFILE=webui", ""));
  agent_session_destroy(multimodal);

  if (had_tz) setenv("TZ", old_tz.c_str(), 1);
  else unsetenv("TZ");
  tzset();
  fs::remove_all(temp_root);
  return 0;
}
