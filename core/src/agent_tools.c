#include "agent/tools.h"

#include "agent_alloc.h"
#include "agent_string.h"

#include <string.h>

typedef struct agent_tool_def {
  char* name;
  char* description;
  char* parameters_json;
} agent_tool_def_t;

struct agent_tool_registry {
  agent_tool_def_t* defs;
  size_t count;
  size_t cap;
};

static void tool_def_destroy(agent_tool_def_t* d) {
  if (!d) {
    return;
  }
  if (d->name) {
    agent_free(d->name);
  }
  if (d->description) {
    agent_free(d->description);
  }
  if (d->parameters_json) {
    agent_free(d->parameters_json);
  }
  d->name = NULL;
  d->description = NULL;
  d->parameters_json = NULL;
}

static agent_status_t registry_reserve(agent_tool_registry_t* r, size_t need_cap) {
  if (need_cap <= r->cap) {
    return AGENT_OK;
  }
  size_t new_cap = r->cap == 0 ? 8 : r->cap;
  while (new_cap < need_cap) {
    new_cap *= 2;
  }
  agent_tool_def_t* new_defs = (agent_tool_def_t*)agent_malloc(new_cap * sizeof(agent_tool_def_t));
  if (!new_defs) {
    return AGENT_ERR_OOM;
  }
  memset(new_defs, 0, new_cap * sizeof(agent_tool_def_t));
  for (size_t i = 0; i < r->count; i++) {
    new_defs[i] = r->defs[i];
  }
  if (r->defs) {
    agent_free(r->defs);
  }
  r->defs = new_defs;
  r->cap = new_cap;
  return AGENT_OK;
}

agent_status_t agent_tool_registry_create(agent_tool_registry_t** out_registry) {
  if (!out_registry) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_registry = NULL;
  agent_tool_registry_t* r = (agent_tool_registry_t*)agent_malloc(sizeof(agent_tool_registry_t));
  if (!r) {
    return AGENT_ERR_OOM;
  }
  r->defs = NULL;
  r->count = 0;
  r->cap = 0;
  *out_registry = r;
  return AGENT_OK;
}

void agent_tool_registry_destroy(agent_tool_registry_t* registry) {
  if (!registry) {
    return;
  }
  for (size_t i = 0; i < registry->count; i++) {
    tool_def_destroy(&registry->defs[i]);
  }
  if (registry->defs) {
    agent_free(registry->defs);
  }
  agent_free(registry);
}

agent_status_t agent_tool_registry_add(
  agent_tool_registry_t* registry,
  const char* name,
  const char* description,
  const char* parameters_json
) {
  if (!registry || !name || !name[0] || !description || !parameters_json) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_status_t st = registry_reserve(registry, registry->count + 1);
  if (st != AGENT_OK) {
    return st;
  }

  agent_tool_def_t* d = &registry->defs[registry->count];
  memset(d, 0, sizeof(*d));
  d->name = agent_strdup(name);
  d->description = agent_strdup(description);
  d->parameters_json = agent_strdup(parameters_json);
  if (!d->name || !d->description || !d->parameters_json) {
    tool_def_destroy(d);
    return AGENT_ERR_OOM;
  }
  registry->count += 1;
  return AGENT_OK;
}

size_t agent_tool_registry_count(const agent_tool_registry_t* registry) {
  return registry ? registry->count : 0;
}

agent_status_t agent_tool_registry_get(const agent_tool_registry_t* registry, size_t index, agent_tool_def_view_t* out_view) {
  if (!registry || !out_view) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (index >= registry->count) {
    return AGENT_ERR_BOUNDS;
  }
  const agent_tool_def_t* d = &registry->defs[index];
  out_view->name = d->name ? d->name : "";
  out_view->description = d->description ? d->description : "";
  out_view->parameters_json = d->parameters_json ? d->parameters_json : "{}";
  return AGENT_OK;
}

