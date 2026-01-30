#include "agent/session_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_msg_eq(const agent_session_t* s, size_t idx, agent_role_t role, const char* content, size_t content_len) {
  agent_message_view_t v = {0};
  assert(agent_session_get_message(s, idx, &v) == AGENT_OK);
  assert(v.role == role);
  assert(v.content_len == content_len);
  assert(memcmp(v.content, content, content_len) == 0);
}

static void test_session_codec_empty_roundtrip(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  agent_string_t text = {0};
  assert(agent_session_codec_encode_v1(s, &text) == AGENT_OK);
  assert(text.data != NULL);
  assert(text.len > 0);

  agent_session_t* decoded = NULL;
  assert(agent_session_codec_decode_v1(text.data, text.len, &decoded) == AGENT_OK);
  assert(agent_session_message_count(decoded) == 0);

  agent_string_free(&text);
  agent_session_destroy(decoded);
  agent_session_destroy(s);
}

static void test_session_codec_roundtrip_escapes(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);

  // Includes: tab, newline, backslash, and a couple of control bytes.
  const char payload[] = "line1\tcol2\nbackslash=\\\\\nctrl=\x01\x7F";
  assert(agent_session_add_message(s, AGENT_ROLE_USER, payload) == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_ASSISTANT, "ok") == AGENT_OK);

  agent_string_t text = {0};
  assert(agent_session_codec_encode_v1(s, &text) == AGENT_OK);

  agent_session_t* decoded = NULL;
  assert(agent_session_codec_decode_v1(text.data, text.len, &decoded) == AGENT_OK);
  assert(agent_session_message_count(decoded) == 2);

  assert_msg_eq(decoded, 0, AGENT_ROLE_USER, payload, strlen(payload));
  assert_msg_eq(decoded, 1, AGENT_ROLE_ASSISTANT, "ok", strlen("ok"));

  agent_string_free(&text);
  agent_session_destroy(decoded);
  agent_session_destroy(s);
}

static void test_session_codec_rejects_bad_header(void) {
  agent_session_t* s = NULL;
  assert(agent_session_codec_decode_v1("NOPE\n", strlen("NOPE\n"), &s) == AGENT_ERR_INTERNAL);
  assert(s == NULL);
}

static void test_session_codec_rejects_bad_escape(void) {
  const char* bad = "AGENT_SESSION\t1\nM\tuser\tbad=\\q\n";
  agent_session_t* s = NULL;
  assert(agent_session_codec_decode_v1(bad, strlen(bad), &s) == AGENT_ERR_INTERNAL);
  assert(s == NULL);
}

void test_session_codec_module(void) {
  test_session_codec_empty_roundtrip();
  test_session_codec_roundtrip_escapes();
  test_session_codec_rejects_bad_header();
  test_session_codec_rejects_bad_escape();
  printf("test_session_codec_module OK\n");
}

