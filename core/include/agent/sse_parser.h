#pragma once

#include "agent/agent.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_sse_event {
  agent_string_t event;  // optional
  agent_string_t id;     // optional
  agent_string_t data;   // concatenated "data:" lines (joined with '\n')
} agent_sse_event_t;

typedef struct agent_sse_parser {
  agent_string_t buf;
  agent_string_t cur_event;
  agent_string_t cur_id;
  agent_string_t cur_data;
} agent_sse_parser_t;

void agent_sse_parser_init(agent_sse_parser_t* parser);
void agent_sse_parser_reset(agent_sse_parser_t* parser);
void agent_sse_parser_free(agent_sse_parser_t* parser);

void agent_sse_event_free(agent_sse_event_t* ev);

// Feed raw bytes from a text/event-stream and emit complete events.
// - out_events is filled with up to events_cap events.
// - out_events_count returns how many events were written.
// Returns AGENT_ERR_LIMIT if more events were parsed than events_cap.
agent_status_t agent_sse_parser_feed(
  agent_sse_parser_t* parser,
  const char* bytes,
  size_t len,
  agent_sse_event_t* out_events,
  size_t events_cap,
  size_t* out_events_count
);

#ifdef __cplusplus
}  // extern "C"
#endif
