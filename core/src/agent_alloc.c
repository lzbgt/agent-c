#include "agent/agent.h"

#include <stdlib.h>

static void* agent_default_malloc(size_t n) { return malloc(n); }
static void agent_default_free(void* p) { free(p); }

static agent_allocator_t g_allocator = {
  .malloc_fn = agent_default_malloc,
  .free_fn = agent_default_free,
};

agent_status_t agent_set_allocator(const agent_allocator_t* allocator) {
  if (allocator == NULL || allocator->malloc_fn == NULL || allocator->free_fn == NULL) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  g_allocator = *allocator;
  return AGENT_OK;
}

void* agent_malloc(size_t n) { return g_allocator.malloc_fn(n); }
void agent_free(void* p) { g_allocator.free_fn(p); }

