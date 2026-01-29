#include "summary_compaction.h"

#include "agent/agent.h"

#include <cassert>
#include <string>

static void test_no_dropped_region_when_short() {
  agent_session_t* s = nullptr;
  assert(agent_session_create(&s) == AGENT_OK);

  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "u1") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "a1") == AGENT_OK);

  SummaryCompactionInput in = build_summary_compaction_input(s, /*keep_last=*/16);
  assert(in.dropped_messages == 0);
  assert(in.excerpt.empty());

  agent_session_destroy(s);
}

static void test_dropped_region_extracts_middle() {
  agent_session_t* s = nullptr;
  assert(agent_session_create(&s) == AGENT_OK);

  // Pinned system prefix (2 messages).
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "sys1") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "sys2") == AGENT_OK);

  // Middle messages that should be dropped when keep_last=2.
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "u1") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "a1") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "u2") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "a2") == AGENT_OK);

  // Suffix to keep.
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "u3") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "a3") == AGENT_OK);

  SummaryCompactionInput in = build_summary_compaction_input(s, /*keep_last=*/2, /*max_excerpt_chars=*/8000);
  assert(in.pinned_system_messages == 2);
  assert(in.kept_suffix_messages == 2);
  assert(in.dropped_messages == 4);
  assert(in.excerpt.find("u1") != std::string::npos);
  assert(in.excerpt.find("a2") != std::string::npos);
  assert(in.excerpt.find("u3") == std::string::npos); // should not include suffix
  assert(in.excerpt.find("sys1") == std::string::npos); // should not include pinned prefix

  agent_session_destroy(s);
}

int main() {
  test_no_dropped_region_when_short();
  test_dropped_region_extracts_middle();
  return 0;
}

