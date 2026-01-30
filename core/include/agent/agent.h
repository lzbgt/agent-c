#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum agent_status {
  AGENT_OK = 0,
  AGENT_ERR_INVALID_ARGUMENT = 1,
  AGENT_ERR_OOM = 2,
  AGENT_ERR_BOUNDS = 3,
  AGENT_ERR_INTERNAL = 4,
  // The provider rejected the request as too large for its context window.
  // Hosts/loops can treat this as a retryable condition after more aggressive compaction.
  AGENT_ERR_CONTEXT_TOO_LONG = 5,
  // Cooperative cancellation requested by the host/user.
  AGENT_ERR_CANCELLED = 6,
  // A configured safety limit was reached (max steps, repetition guard, etc).
  AGENT_ERR_LIMIT = 7,
} agent_status_t;

typedef enum agent_role {
  AGENT_ROLE_SYSTEM = 0,
  AGENT_ROLE_USER = 1,
  AGENT_ROLE_ASSISTANT = 2,
  AGENT_ROLE_TOOL = 3,
} agent_role_t;

typedef struct agent_allocator {
  void* (*malloc_fn)(size_t size);
  void (*free_fn)(void* ptr);
} agent_allocator_t;

// Optional: call once at process startup. If not called, libc malloc/free are used.
agent_status_t agent_set_allocator(const agent_allocator_t* allocator);

// Convenience allocation helpers using the active allocator (either libc or the allocator installed via
// agent_set_allocator). These are primarily intended for cross-boundary allocations where the core will free
// memory that a host/provider allocated.
void* agent_malloc(size_t size);
void agent_free(void* ptr);

typedef struct agent_session agent_session_t;

typedef struct agent_message_view {
  agent_role_t role;
  const char* content;     // owned by the session
  size_t content_len;      // bytes, excluding terminator
} agent_message_view_t;

typedef struct agent_string {
  char* data;
  size_t len; // bytes, excluding terminator
} agent_string_t;

agent_status_t agent_string_set_copy(agent_string_t* s, const char* data, size_t len);
void agent_string_free(agent_string_t* s);

agent_status_t agent_session_create(agent_session_t** out_session);
void agent_session_destroy(agent_session_t* session);

agent_status_t agent_session_add_message(agent_session_t* session, agent_role_t role, const char* content);

size_t agent_session_message_count(const agent_session_t* session);
agent_status_t agent_session_get_message(const agent_session_t* session, size_t index, agent_message_view_t* out_view);

// Rough, portable estimate used for char-budget compaction.
size_t agent_session_estimated_chars(const agent_session_t* session);

typedef struct agent_compact_report {
  size_t before_chars;
  size_t after_chars;
  size_t dropped_messages;
  uint8_t inserted_summary;
} agent_compact_report_t;

// Convention: host-generated compaction summaries (inserted as system messages) should start with
// this prefix. The core treats such messages as *not pinned* so they can be replaced/compacted
// over time (otherwise pinned system prefixes could grow without bound).
#define AGENT_SESSION_SUMMARY_PREFIX "AGENT_SESSION_SUMMARY:"

// Compacts to satisfy `max_chars` by preserving pinned system messages (leading system messages)
// plus the last `keep_last_messages` messages. If `summary_or_null` is provided and non-empty,
// it is inserted as a system message after pinned system messages.
agent_status_t agent_session_compact_char_budget(
  agent_session_t* session,
  size_t max_chars,
  size_t keep_last_messages,
  const char* summary_or_null,
  agent_compact_report_t* out_report
);

const char* agent_role_to_string(agent_role_t role);
agent_status_t agent_role_from_string(const char* s, agent_role_t* out_role);

#ifdef __cplusplus
}  // extern "C"
#endif
