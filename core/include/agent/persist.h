#pragma once

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional persistence port for hosts/embedded targets.
//
// The core remains storage-agnostic. Hosts can implement this interface using:
// - filesystem files (e.g. .sess)
// - SQLite
// - NVS/flash key-value stores
//
// This interface intentionally avoids cross-boundary allocations by using a sink callback for listing.

typedef void (*agent_session_id_sink_fn)(void* sink_ctx, const char* session_id);

typedef agent_status_t (*agent_persist_load_fn)(void* ctx, const char* session_id, agent_session_t** out_session);
typedef agent_status_t (*agent_persist_save_fn)(void* ctx, const char* session_id, const agent_session_t* session);
typedef agent_status_t (*agent_persist_delete_fn)(void* ctx, const char* session_id);
typedef agent_status_t (*agent_persist_list_fn)(void* ctx, agent_session_id_sink_fn sink, void* sink_ctx);
typedef void (*agent_persist_destroy_fn)(void* ctx);

typedef struct agent_persistor {
  void* ctx;
  agent_persist_load_fn load;
  agent_persist_save_fn save;
  agent_persist_delete_fn del;
  agent_persist_list_fn list;      // optional (may be NULL)
  agent_persist_destroy_fn destroy; // optional (may be NULL)
} agent_persistor_t;

static inline void agent_persistor_destroy(agent_persistor_t* p) {
  if (!p) return;
  if (p->destroy && p->ctx) {
    p->destroy(p->ctx);
  }
  p->ctx = NULL;
  p->load = NULL;
  p->save = NULL;
  p->del = NULL;
  p->list = NULL;
  p->destroy = NULL;
}

#ifdef __cplusplus
}  // extern "C"
#endif

