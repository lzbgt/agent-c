#pragma once

#include "agent/agent.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum agent_part_type {
  AGENT_PART_TEXT = 0,
  AGENT_PART_IMAGE_URL = 1,
  AGENT_PART_AUDIO_URL = 2,
  AGENT_PART_VIDEO_URL = 3,
  AGENT_PART_BINARY = 4, // bytes + mime
} agent_part_type_t;

// Input to add parts: non-owning views (core copies data).
typedef struct agent_content_part {
  agent_part_type_t type;
  const char* text_or_null;      // for TEXT
  const char* url_or_null;       // for *_URL
  const uint8_t* bytes_or_null;  // for BINARY
  size_t bytes_len;
  const char* mime_or_null;      // optional (recommended for BINARY)
} agent_content_part_t;

// Output view: pointers remain valid while the session/message exists.
typedef struct agent_content_part_view {
  agent_part_type_t type;
  const char* text;
  size_t text_len;
  const char* url;
  size_t url_len;
  const uint8_t* bytes;
  size_t bytes_len;
  const char* mime;
  size_t mime_len;
} agent_content_part_view_t;

agent_status_t agent_session_add_message_parts(
  agent_session_t* session,
  agent_role_t role,
  const agent_content_part_t* parts,
  size_t part_count
);

size_t agent_session_message_part_count(const agent_session_t* session, size_t message_index);

agent_status_t agent_session_get_message_part(
  const agent_session_t* session,
  size_t message_index,
  size_t part_index,
  agent_content_part_view_t* out_view
);

#ifdef __cplusplus
}  // extern "C"
#endif

