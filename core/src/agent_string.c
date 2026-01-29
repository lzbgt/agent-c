#include "agent_string.h"

#include "agent/agent.h"
#include "agent_alloc.h"

#include <string.h>

char* agent_strdup(const char* s) {
  if (s == NULL) {
    return NULL;
  }
  const size_t n = strlen(s);
  char* out = (char*)agent_malloc(n + 1);
  if (!out) {
    return NULL;
  }
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

agent_status_t agent_string_set_copy(agent_string_t* s, const char* data, size_t len) {
  if (!s || (!data && len != 0)) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_string_free(s);
  char* buf = (char*)agent_malloc(len + 1);
  if (!buf) {
    return AGENT_ERR_OOM;
  }
  if (len) {
    memcpy(buf, data, len);
  }
  buf[len] = '\0';
  s->data = buf;
  s->len = len;
  return AGENT_OK;
}

void agent_string_free(agent_string_t* s) {
  if (!s) {
    return;
  }
  if (s->data) {
    agent_free(s->data);
  }
  s->data = NULL;
  s->len = 0;
}
