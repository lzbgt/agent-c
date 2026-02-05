#include "agent/agent.h"
#include "agent/parts.h"
#include "agent/tools.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_compaction_preserves_pinned_and_suffix(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  // Pinned system prefix.
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "You are a helpful assistant.") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned rule #2.") == AGENT_OK);

  // Many user/assistant turns.
  for (int i = 0; i < 50; i++) {
    assert(agent_session_add_message(s, AGENT_ROLE_USER, "User says hello hello hello hello hello.") == AGENT_OK);
    assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "Assistant replies blah blah blah blah blah.") == AGENT_OK);
  }

  const size_t before = agent_session_estimated_chars(s);
  assert(before > 0);

  agent_compact_report_t rep = {0};
  assert(agent_session_compact_char_budget(s, 800, 6, NULL, &rep) == AGENT_OK);
  assert(rep.before_chars == before);
  assert(rep.after_chars == agent_session_estimated_chars(s));
  assert(rep.after_chars <= 800 || rep.after_chars == before);
  assert(agent_session_message_count(s) >= 2); // pinned still present

  agent_message_view_t v0 = {0};
  agent_message_view_t v1 = {0};
  assert(agent_session_get_message(s, 0, &v0) == AGENT_OK);
  assert(agent_session_get_message(s, 1, &v1) == AGENT_OK);
  assert(v0.role == AGENT_ROLE_SYSTEM);
  assert(v1.role == AGENT_ROLE_SYSTEM);

  agent_session_destroy(s);
}

static void test_compaction_overlap_prefix_suffix_does_not_drop_first_system(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned system") == AGENT_OK);
  for (int i = 0; i < 10; i++) {
    assert(agent_session_add_message(s, AGENT_ROLE_USER, "U") == AGENT_OK);
    assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "A") == AGENT_OK);
  }

  const size_t count_before = agent_session_message_count(s);
  const size_t chars_before = agent_session_estimated_chars(s);
  assert(count_before > 0);
  assert(chars_before > 0);

  // keep_last == count forces prefix/suffix overlap (no droppable "middle" region).
  // This is an edge case: compaction may be unable to reach max_chars, but it must not
  // delete the first (non-summary) system message.
  agent_compact_report_t rep = {0};
  assert(agent_session_compact_char_budget(s, 8, count_before, NULL, &rep) == AGENT_OK);
  assert(rep.after_chars == agent_session_estimated_chars(s));

  agent_message_view_t v0 = {0};
  assert(agent_session_get_message(s, 0, &v0) == AGENT_OK);
  assert(v0.role == AGENT_ROLE_SYSTEM);
  assert(v0.content_len == strlen("Pinned system"));
  assert(strncmp(v0.content, "Pinned system", v0.content_len) == 0);

  agent_session_destroy(s);
}

static void test_compaction_all_system_messages_preserves_first_system(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "System 0") == AGENT_OK);
  for (int i = 0; i < 40; i++) {
    assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "System rule blah blah blah blah blah") == AGENT_OK);
  }

  agent_compact_report_t rep = {0};
  assert(agent_session_compact_char_budget(s, 120, 2, NULL, &rep) == AGENT_OK);
  assert(rep.after_chars == agent_session_estimated_chars(s));

  agent_message_view_t v0 = {0};
  assert(agent_session_get_message(s, 0, &v0) == AGENT_OK);
  assert(v0.role == AGENT_ROLE_SYSTEM);
  assert(v0.content_len == strlen("System 0"));
  assert(strncmp(v0.content, "System 0", v0.content_len) == 0);
  assert(rep.dropped_messages > 0);

  agent_session_destroy(s);
}

static void test_compaction_empty_session_is_ok(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);
  assert(agent_session_message_count(s) == 0);

  agent_compact_report_t rep = {0};
  assert(agent_session_compact_char_budget(s, 32, 4, NULL, &rep) == AGENT_OK);
  assert(rep.after_chars == 0);
  assert(agent_session_message_count(s) == 0);

  agent_session_destroy(s);
}

static void test_compaction_with_summary_inserts_after_pinned(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned A") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned B") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "Hello") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "Hi") == AGENT_OK);

  agent_compact_report_t rep = {0};
  const char* summary = AGENT_SESSION_SUMMARY_PREFIX "Session summary: ...";
  assert(agent_session_compact_char_budget(s, 1024, 2, summary, &rep) == AGENT_OK);
  assert(rep.after_chars == agent_session_estimated_chars(s));
  assert(rep.inserted_summary == 1);

  agent_message_view_t v2 = {0};
  assert(agent_session_get_message(s, 2, &v2) == AGENT_OK);
  assert(v2.role == AGENT_ROLE_SYSTEM);
  assert(v2.content_len >= strlen(AGENT_SESSION_SUMMARY_PREFIX));
  assert(strncmp(v2.content, AGENT_SESSION_SUMMARY_PREFIX, strlen(AGENT_SESSION_SUMMARY_PREFIX)) == 0);

  agent_session_destroy(s);
}

static void test_summary_marker_is_not_pinned(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  // One pinned system message.
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned A") == AGENT_OK);
  // Insert a host-style summary marker. This should NOT be treated as pinned on future compactions.
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, AGENT_SESSION_SUMMARY_PREFIX "\nOld summary") == AGENT_OK);

  // Add enough content so compaction has to drop something.
  for (int i = 0; i < 20; i++) {
    assert(agent_session_add_message(s, AGENT_ROLE_USER, "User says hello hello hello hello hello.") == AGENT_OK);
    assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "Assistant replies blah blah blah blah blah.") == AGENT_OK);
  }

  // Tight budget; keep a small suffix. The summary marker should be droppable.
  agent_compact_report_t rep = {0};
  assert(agent_session_compact_char_budget(s, 400, 4, NULL, &rep) == AGENT_OK);
  assert(rep.after_chars == agent_session_estimated_chars(s));

  agent_message_view_t v0 = {0};
  assert(agent_session_get_message(s, 0, &v0) == AGENT_OK);
  assert(v0.role == AGENT_ROLE_SYSTEM);

  // After compaction, the second message should not be the summary marker (it should have been droppable).
  if (agent_session_message_count(s) > 1) {
    agent_message_view_t v1 = {0};
    assert(agent_session_get_message(s, 1, &v1) == AGENT_OK);
    if (v1.role == AGENT_ROLE_SYSTEM && v1.content_len >= strlen(AGENT_SESSION_SUMMARY_PREFIX)) {
      assert(strncmp(v1.content, AGENT_SESSION_SUMMARY_PREFIX, strlen(AGENT_SESSION_SUMMARY_PREFIX)) != 0);
    }
  }

  agent_session_destroy(s);
}

static void test_message_parts_roundtrip(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  agent_content_part_t parts[2];
  parts[0].type = AGENT_PART_TEXT;
  parts[0].text_or_null = "hello";
  parts[0].url_or_null = NULL;
  parts[0].bytes_or_null = NULL;
  parts[0].bytes_len = 0;
  parts[0].mime_or_null = NULL;

  parts[1].type = AGENT_PART_IMAGE_URL;
  parts[1].text_or_null = NULL;
  parts[1].url_or_null = "https://example.com/x.png";
  parts[1].bytes_or_null = NULL;
  parts[1].bytes_len = 0;
  parts[1].mime_or_null = NULL;

  assert(agent_session_add_message_parts(s, AGENT_ROLE_USER, parts, 2) == AGENT_OK);
  assert(agent_session_message_count(s) == 1);

  agent_message_view_t msg = {0};
  assert(agent_session_get_message(s, 0, &msg) == AGENT_OK);
  assert(msg.role == AGENT_ROLE_USER);
  assert(msg.content_len == 5);

  assert(agent_session_message_part_count(s, 0) == 2);
  agent_content_part_view_t p0 = {0};
  agent_content_part_view_t p1 = {0};
  assert(agent_session_get_message_part(s, 0, 0, &p0) == AGENT_OK);
  assert(agent_session_get_message_part(s, 0, 1, &p1) == AGENT_OK);
  assert(p0.type == AGENT_PART_TEXT);
  assert(p1.type == AGENT_PART_IMAGE_URL);
  assert(p1.url_len > 0);

  agent_session_destroy(s);
}

static void test_tool_registry_roundtrip(void) {
  agent_tool_registry_t* r = NULL;
  assert(agent_tool_registry_create(&r) == AGENT_OK);
  assert(agent_tool_registry_count(r) == 0);

  const char* params = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}},\"required\":[\"x\"]}";
  assert(agent_tool_registry_add(r, "gpio_set", "Set GPIO pin output", params) == AGENT_OK);
  assert(agent_tool_registry_count(r) == 1);

  agent_tool_def_view_t v = {0};
  assert(agent_tool_registry_get(r, 0, &v) == AGENT_OK);
  assert(v.name && v.name[0]);
  assert(v.description && v.description[0]);
  assert(v.parameters_json && v.parameters_json[0]);

  agent_tool_registry_destroy(r);
}

int main(void) {
  test_compaction_preserves_pinned_and_suffix();
  test_compaction_overlap_prefix_suffix_does_not_drop_first_system();
  test_compaction_all_system_messages_preserves_first_system();
  test_compaction_empty_session_is_ok();
  test_compaction_with_summary_inserts_after_pinned();
  test_summary_marker_is_not_pinned();
  test_message_parts_roundtrip();
  test_tool_registry_roundtrip();
  extern void test_session_codec_module(void);
  test_session_codec_module();
  extern void test_runner_module(void);
  test_runner_module();
  extern void test_tool_loop_module(void);
  test_tool_loop_module();
  extern void test_tool_loop_memory_flush_module(void);
  test_tool_loop_memory_flush_module();
  extern void test_edge_interop_module(void);
  test_edge_interop_module();
  extern void test_json_c14n_module(void);
  test_json_c14n_module();
  extern void test_hmac_sha256_module(void);
  test_hmac_sha256_module();
  extern void test_ed25519_module(void);
  test_ed25519_module();
  extern void test_cbor_det_module(void);
  test_cbor_det_module();
  extern void test_cbor_read_module(void);
  test_cbor_read_module();
  extern void test_base64_module(void);
  test_base64_module();
  extern void test_umbmp_auth_core_module(void);
  test_umbmp_auth_core_module();
  extern void test_umbmp_envelope_read_module(void);
  test_umbmp_envelope_read_module();
  extern void test_um_eais_task_assign_read_module(void);
  test_um_eais_task_assign_read_module();
  extern void test_umbmp_envelope_write_module(void);
  test_umbmp_envelope_write_module();
  extern void test_um_eais_outbox_read_module(void);
  test_um_eais_outbox_read_module();
  extern void test_um_eais_platform_caps_req_read_module(void);
  test_um_eais_platform_caps_req_read_module();
  printf("agent_core_tests OK\n");
  return 0;
}
