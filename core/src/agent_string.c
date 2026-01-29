#include "agent_string.h"

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

