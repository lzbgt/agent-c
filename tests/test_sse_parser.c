#include "agent/sse_parser.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

static void expect_string(const agent_string_t* s, const char* want) {
  if (!want) want = "";
  const size_t want_len = strlen(want);
  assert(s != NULL);
  assert(s->len == want_len);
  if (want_len > 0) {
    assert(s->data != NULL);
    assert(memcmp(s->data, want, want_len) == 0);
  }
}

static void test_sse_single_event_split(void) {
  agent_sse_parser_t parser;
  agent_sse_parser_init(&parser);

  agent_sse_event_t events[4] = {0};
  size_t count = 0;

  const char* part1 = "data: Hello";
  assert(agent_sse_parser_feed(&parser, part1, strlen(part1), events, 4, &count) == AGENT_OK);
  assert(count == 0);

  const char* part2 = " world\n\n";
  assert(agent_sse_parser_feed(&parser, part2, strlen(part2), events, 4, &count) == AGENT_OK);
  assert(count == 1);
  expect_string(&events[0].event, "");
  expect_string(&events[0].id, "");
  expect_string(&events[0].data, "Hello world");
  agent_sse_event_free(&events[0]);

  agent_sse_parser_free(&parser);
}

static void test_sse_multiline_fields(void) {
  agent_sse_parser_t parser;
  agent_sse_parser_init(&parser);

  agent_sse_event_t events[4] = {0};
  size_t count = 0;

  const char* payload =
    "event: update\n"
    "id: 42\n"
    "data: line1\n"
    "data: line2\n"
    "\n";
  assert(agent_sse_parser_feed(&parser, payload, strlen(payload), events, 4, &count) == AGENT_OK);
  assert(count == 1);
  expect_string(&events[0].event, "update");
  expect_string(&events[0].id, "42");
  expect_string(&events[0].data, "line1\nline2");
  agent_sse_event_free(&events[0]);

  agent_sse_parser_free(&parser);
}

void test_sse_parser_module(void) {
  test_sse_single_event_split();
  test_sse_multiline_fields();
  // Network coalescing must not lose events after a bounded output batch.
  agent_sse_parser_t parser;
  agent_sse_parser_init(&parser);
  const char* burst = "data: one\n\ndata: two\n\ndata: three\n\ndata: four\n\ndata: partial";
  agent_sse_event_t events[2] = {0};
  size_t count = 0;
  assert(agent_sse_parser_feed(&parser,burst,strlen(burst),events,2,&count) == AGENT_ERR_LIMIT);
  assert(count == 2);
  expect_string(&events[0].data,"one"); expect_string(&events[1].data,"two");
  agent_sse_event_free(&events[0]); agent_sse_event_free(&events[1]);
  assert(agent_sse_parser_feed(&parser,NULL,0,events,2,&count) == AGENT_OK && count == 2);
  expect_string(&events[0].data,"three"); expect_string(&events[1].data,"four");
  agent_sse_event_free(&events[0]); agent_sse_event_free(&events[1]);
  assert(agent_sse_parser_feed(&parser," end\n\n",6,events,2,&count) == AGENT_OK && count == 1);
  expect_string(&events[0].data,"partial end");
  agent_sse_event_free(&events[0]); agent_sse_parser_free(&parser);
  printf("test_sse_parser_module OK\n");
}
