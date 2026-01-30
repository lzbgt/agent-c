#include "agent/tool_provider.h"

#include "agent_alloc.h"

#include <string.h>

static void agent_tool_call_free(agent_tool_call_t* c) {
  if (!c) return;
  agent_string_free(&c->id);
  agent_string_free(&c->name);
  agent_string_free(&c->arguments_json);
  memset(c, 0, sizeof(*c));
}

void agent_tool_provider_response_free(agent_tool_provider_response_t* r) {
  if (!r) return;
  agent_string_free(&r->assistant_content);
  if (r->tool_calls) {
    for (size_t i = 0; i < r->tool_call_count; i++) {
      agent_tool_call_free(&r->tool_calls[i]);
    }
    agent_free(r->tool_calls);
  }
  agent_string_free(&r->error_message);
  memset(r, 0, sizeof(*r));
}

