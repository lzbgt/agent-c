#include "agent/agent.h"
#include "agent/parts.h"

#include "agent_alloc.h"
#include "agent_string.h"

#include <string.h>

typedef struct agent_part {
  agent_part_type_t type;
  char* text;
  size_t text_len;
  char* url;
  size_t url_len;
  uint8_t* bytes;
  size_t bytes_len;
  char* mime;
  size_t mime_len;
} agent_part_t;

typedef struct agent_message {
  agent_role_t role;
  char* content;
  size_t content_len;
  agent_part_t* parts;
  size_t part_count;
  size_t part_cap;
} agent_message_t;

struct agent_session {
  agent_message_t* messages;
  size_t count;
  size_t cap;
};

static void agent_part_destroy(agent_part_t* part) {
  if (!part) {
    return;
  }
  if (part->text) {
    agent_free(part->text);
    part->text = NULL;
  }
  if (part->url) {
    agent_free(part->url);
    part->url = NULL;
  }
  if (part->bytes) {
    agent_free(part->bytes);
    part->bytes = NULL;
  }
  if (part->mime) {
    agent_free(part->mime);
    part->mime = NULL;
  }
  part->text_len = 0;
  part->url_len = 0;
  part->bytes_len = 0;
  part->mime_len = 0;
}

static void agent_message_destroy(agent_message_t* msg) {
  if (!msg) {
    return;
  }
  if (msg->content) {
    agent_free(msg->content);
    msg->content = NULL;
  }
  if (msg->parts) {
    for (size_t i = 0; i < msg->part_count; i++) {
      agent_part_destroy(&msg->parts[i]);
    }
    agent_free(msg->parts);
    msg->parts = NULL;
  }
  msg->content_len = 0;
  msg->part_count = 0;
  msg->part_cap = 0;
}

static agent_status_t agent_session_reserve(agent_session_t* s, size_t need_cap) {
  if (need_cap <= s->cap) {
    return AGENT_OK;
  }
  size_t new_cap = s->cap == 0 ? 8 : s->cap;
  while (new_cap < need_cap) {
    new_cap *= 2;
  }

  agent_message_t* new_messages = (agent_message_t*)agent_malloc(new_cap * sizeof(agent_message_t));
  if (!new_messages) {
    return AGENT_ERR_OOM;
  }
  memset(new_messages, 0, new_cap * sizeof(agent_message_t));

  for (size_t i = 0; i < s->count; i++) {
    new_messages[i] = s->messages[i];
  }
  if (s->messages) {
    agent_free(s->messages);
  }
  s->messages = new_messages;
  s->cap = new_cap;
  return AGENT_OK;
}

agent_status_t agent_session_create(agent_session_t** out_session) {
  if (!out_session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_session = NULL;

  agent_session_t* s = (agent_session_t*)agent_malloc(sizeof(agent_session_t));
  if (!s) {
    return AGENT_ERR_OOM;
  }
  s->messages = NULL;
  s->count = 0;
  s->cap = 0;

  *out_session = s;
  return AGENT_OK;
}

void agent_session_destroy(agent_session_t* session) {
  if (!session) {
    return;
  }
  for (size_t i = 0; i < session->count; i++) {
    agent_message_destroy(&session->messages[i]);
  }
  if (session->messages) {
    agent_free(session->messages);
  }
  agent_free(session);
}

agent_status_t agent_session_add_message(agent_session_t* session, agent_role_t role, const char* content) {
  if (!session || !content) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_status_t st = agent_session_reserve(session, session->count + 1);
  if (st != AGENT_OK) {
    return st;
  }

  char* owned = agent_strdup(content);
  if (!owned) {
    return AGENT_ERR_OOM;
  }
  const size_t len = strlen(owned);

  agent_message_t* msg = &session->messages[session->count];
  msg->role = role;
  msg->content = owned;
  msg->content_len = len;
  msg->parts = NULL;
  msg->part_count = 0;
  msg->part_cap = 0;
  session->count += 1;
  return AGENT_OK;
}

size_t agent_session_message_count(const agent_session_t* session) {
  return session ? session->count : 0;
}

agent_status_t agent_session_get_message(const agent_session_t* session, size_t index, agent_message_view_t* out_view) {
  if (!session || !out_view) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (index >= session->count) {
    return AGENT_ERR_BOUNDS;
  }
  const agent_message_t* msg = &session->messages[index];
  out_view->role = msg->role;
  out_view->content = msg->content ? msg->content : "";
  out_view->content_len = msg->content_len;
  return AGENT_OK;
}

size_t agent_session_estimated_chars(const agent_session_t* session) {
  if (!session) {
    return 0;
  }
  size_t sum = 0;
  for (size_t i = 0; i < session->count; i++) {
    const agent_message_t* msg = &session->messages[i];
    // Conservative overhead per message for role labels / separators.
    sum += 16;
    sum += msg->content_len;
    for (size_t j = 0; j < msg->part_count; j++) {
      // Add a small overhead plus any binary payload size (worst-case).
      sum += 8;
      sum += msg->parts[j].bytes_len;
      sum += msg->parts[j].url_len;
    }
  }
  return sum;
}

static uint8_t agent_message_is_summary_marker(const agent_message_t* msg) {
  if (!msg || msg->role != AGENT_ROLE_SYSTEM) {
    return 0;
  }
  const char* c = msg->content ? msg->content : "";
  return (strncmp(c, AGENT_SESSION_SUMMARY_PREFIX, strlen(AGENT_SESSION_SUMMARY_PREFIX)) == 0) ? 1 : 0;
}

static uint8_t agent_session_has_pinnable_first_system_message(const agent_session_t* session) {
  if (!session || session->count == 0) {
    return 0;
  }
  const agent_message_t* m0 = &session->messages[0];
  if (m0->role != AGENT_ROLE_SYSTEM) {
    return 0;
  }
  if (agent_message_is_summary_marker(m0)) {
    return 0;
  }
  return 1;
}

static size_t agent_message_estimated_chars(const agent_message_t* msg) {
  if (!msg) {
    return 0;
  }
  size_t sum = 16 + msg->content_len;
  for (size_t j = 0; j < msg->part_count; j++) {
    sum += 8;
    sum += msg->parts[j].bytes_len;
    sum += msg->parts[j].url_len;
  }
  return sum;
}

static size_t agent_count_pinned_system_prefix(const agent_session_t* session) {
  size_t pinned = 0;
  for (size_t i = 0; i < session->count; i++) {
    if (session->messages[i].role == AGENT_ROLE_SYSTEM) {
      // Do not pin host-generated compaction summaries; they should be replaceable over time.
      if (agent_message_is_summary_marker(&session->messages[i])) {
        break;
      }
      pinned += 1;
      continue;
    }
    break;
  }
  return pinned;
}

static agent_status_t agent_session_insert_message(agent_session_t* session, size_t index, agent_role_t role, const char* content) {
  if (!session || !content) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (index > session->count) {
    return AGENT_ERR_BOUNDS;
  }
  agent_status_t st = agent_session_reserve(session, session->count + 1);
  if (st != AGENT_OK) {
    return st;
  }
  // Move tail one slot right.
  for (size_t i = session->count; i > index; i--) {
    session->messages[i] = session->messages[i - 1];
  }
  memset(&session->messages[index], 0, sizeof(agent_message_t));
  session->count += 1;
  // Fill inserted message.
  char* owned = agent_strdup(content);
  if (!owned) {
    return AGENT_ERR_OOM;
  }
  session->messages[index].role = role;
  session->messages[index].content = owned;
  session->messages[index].content_len = strlen(owned);
  session->messages[index].parts = NULL;
  session->messages[index].part_count = 0;
  session->messages[index].part_cap = 0;
  return AGENT_OK;
}

static void agent_session_drop_range(agent_session_t* session, size_t start, size_t end_exclusive) {
  if (!session || start >= end_exclusive || end_exclusive > session->count) {
    return;
  }
  for (size_t i = start; i < end_exclusive; i++) {
    agent_message_destroy(&session->messages[i]);
  }
  const size_t tail = session->count - end_exclusive;
  for (size_t i = 0; i < tail; i++) {
    session->messages[start + i] = session->messages[end_exclusive + i];
  }
  // Zero out moved-from slots to avoid double frees if something goes wrong later.
  for (size_t i = session->count - (end_exclusive - start); i < session->count; i++) {
    memset(&session->messages[i], 0, sizeof(agent_message_t));
  }
  session->count -= (end_exclusive - start);
}

agent_status_t agent_session_compact_char_budget(
  agent_session_t* session,
  size_t max_chars,
  size_t keep_last_messages,
  const char* summary_or_null,
  agent_compact_report_t* out_report
) {
  if (!session || max_chars == 0) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  agent_compact_report_t report = {0};
  size_t current_chars = agent_session_estimated_chars(session);
  report.before_chars = current_chars;
  report.after_chars = current_chars;

  const uint8_t want_summary = (summary_or_null != NULL && summary_or_null[0] != '\0');

  size_t pinned = agent_count_pinned_system_prefix(session);
  const size_t pinned_min = agent_session_has_pinnable_first_system_message(session) ? 1u : 0u;
  if (pinned == 0 && pinned_min == 1u) {
    pinned = 1;
  }

  if (want_summary) {
    agent_status_t st = agent_session_insert_message(session, pinned, AGENT_ROLE_SYSTEM, summary_or_null);
    if (st != AGENT_OK) {
      return st;
    }
    report.inserted_summary = 1;
    // Add the inserted summary size to our rolling estimate.
    current_chars += agent_message_estimated_chars(&session->messages[pinned]);
  }

  if (current_chars <= max_chars) {
    report.after_chars = current_chars;
    if (out_report) {
      *out_report = report;
    }
    return AGENT_OK;
  }

  // Determine protected suffix window.
  size_t keep_last = keep_last_messages;
  if (keep_last > session->count) {
    keep_last = session->count;
  }
  // First try to keep the full pinned prefix (+ inserted summary), but never protect fewer than pinned_min.
  const size_t protected_prefix_pref = pinned + (want_summary ? 1u : 0u);
  const size_t protected_prefix_min = pinned_min;

  size_t dropped = 0;
  while (session->count > 0 && current_chars > max_chars) {
    const size_t current_keep_last = keep_last > session->count ? session->count : keep_last;
    const size_t current_suffix_start = session->count - current_keep_last;

    size_t drop_index = (size_t)-1;
    if (protected_prefix_pref < current_suffix_start) {
      drop_index = protected_prefix_pref;
    } else if (protected_prefix_min < current_suffix_start) {
      // Overlap between preferred prefix and suffix. As a fallback, allow dropping from after the
      // first pinnable system message so compaction can still make progress without deleting that first message.
      drop_index = protected_prefix_min;
    } else {
      // Only the protected prefix and/or suffix remain; we can't delete anything without violating invariants.
      break;
    }

    const size_t dropped_chars = agent_message_estimated_chars(&session->messages[drop_index]);

    agent_session_drop_range(session, drop_index, drop_index + 1);
    dropped += 1;
    if (current_chars >= dropped_chars) {
      current_chars -= dropped_chars;
    } else {
      current_chars = agent_session_estimated_chars(session); // best-effort resync
    }
  }

  report.dropped_messages = dropped;
  report.after_chars = current_chars;
  if (out_report) {
    *out_report = report;
  }
  return AGENT_OK;
}

const char* agent_role_to_string(agent_role_t role) {
  switch (role) {
    case AGENT_ROLE_SYSTEM:
      return "system";
    case AGENT_ROLE_USER:
      return "user";
    case AGENT_ROLE_ASSISTANT:
      return "assistant";
    case AGENT_ROLE_TOOL:
      return "tool";
    default:
      return "unknown";
  }
}

agent_status_t agent_role_from_string(const char* s, agent_role_t* out_role) {
  if (!s || !out_role) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (strcmp(s, "system") == 0) {
    *out_role = AGENT_ROLE_SYSTEM;
    return AGENT_OK;
  }
  if (strcmp(s, "user") == 0) {
    *out_role = AGENT_ROLE_USER;
    return AGENT_OK;
  }
  if (strcmp(s, "assistant") == 0) {
    *out_role = AGENT_ROLE_ASSISTANT;
    return AGENT_OK;
  }
  if (strcmp(s, "tool") == 0) {
    *out_role = AGENT_ROLE_TOOL;
    return AGENT_OK;
  }
  return AGENT_ERR_INVALID_ARGUMENT;
}

static agent_status_t agent_message_reserve_parts(agent_message_t* msg, size_t need_cap) {
  if (need_cap <= msg->part_cap) {
    return AGENT_OK;
  }
  size_t new_cap = msg->part_cap == 0 ? 4 : msg->part_cap;
  while (new_cap < need_cap) {
    new_cap *= 2;
  }
  agent_part_t* new_parts = (agent_part_t*)agent_malloc(new_cap * sizeof(agent_part_t));
  if (!new_parts) {
    return AGENT_ERR_OOM;
  }
  memset(new_parts, 0, new_cap * sizeof(agent_part_t));
  for (size_t i = 0; i < msg->part_count; i++) {
    new_parts[i] = msg->parts[i];
  }
  if (msg->parts) {
    agent_free(msg->parts);
  }
  msg->parts = new_parts;
  msg->part_cap = new_cap;
  return AGENT_OK;
}

static agent_status_t agent_part_copy(agent_part_t* dst, const agent_content_part_t* src) {
  memset(dst, 0, sizeof(*dst));
  dst->type = src->type;
  if (src->text_or_null) {
    dst->text = agent_strdup(src->text_or_null);
    if (!dst->text) {
      return AGENT_ERR_OOM;
    }
    dst->text_len = strlen(dst->text);
  }
  if (src->url_or_null) {
    dst->url = agent_strdup(src->url_or_null);
    if (!dst->url) {
      agent_part_destroy(dst);
      return AGENT_ERR_OOM;
    }
    dst->url_len = strlen(dst->url);
  }
  if (src->mime_or_null) {
    dst->mime = agent_strdup(src->mime_or_null);
    if (!dst->mime) {
      agent_part_destroy(dst);
      return AGENT_ERR_OOM;
    }
    dst->mime_len = strlen(dst->mime);
  }
  if (src->bytes_or_null && src->bytes_len) {
    uint8_t* b = (uint8_t*)agent_malloc(src->bytes_len);
    if (!b) {
      agent_part_destroy(dst);
      return AGENT_ERR_OOM;
    }
    memcpy(b, src->bytes_or_null, src->bytes_len);
    dst->bytes = b;
    dst->bytes_len = src->bytes_len;
  }
  return AGENT_OK;
}

static agent_status_t agent_message_derive_text_content(agent_message_t* msg) {
  // Compatibility: keep msg->content as concatenation of TEXT parts (joined by '\n').
  // If no TEXT parts exist, content is an empty string.
  size_t needed = 0;
  size_t text_parts = 0;
  for (size_t i = 0; i < msg->part_count; i++) {
    if (msg->parts[i].type == AGENT_PART_TEXT && msg->parts[i].text) {
      needed += msg->parts[i].text_len;
      text_parts += 1;
    }
  }
  if (text_parts > 1) {
    needed += (text_parts - 1); // '\n' separators
  }

  char* buf = (char*)agent_malloc(needed + 1);
  if (!buf) {
    return AGENT_ERR_OOM;
  }
  size_t off = 0;
  size_t written_parts = 0;
  for (size_t i = 0; i < msg->part_count; i++) {
    if (msg->parts[i].type != AGENT_PART_TEXT || !msg->parts[i].text) {
      continue;
    }
    if (written_parts) {
      buf[off++] = '\n';
    }
    memcpy(buf + off, msg->parts[i].text, msg->parts[i].text_len);
    off += msg->parts[i].text_len;
    written_parts += 1;
  }
  buf[off] = '\0';

  if (msg->content) {
    agent_free(msg->content);
  }
  msg->content = buf;
  msg->content_len = needed;
  return AGENT_OK;
}

agent_status_t agent_session_add_message_parts(
  agent_session_t* session,
  agent_role_t role,
  const agent_content_part_t* parts,
  size_t part_count
) {
  if (!session || !parts || part_count == 0) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_status_t st = agent_session_reserve(session, session->count + 1);
  if (st != AGENT_OK) {
    return st;
  }

  agent_message_t* msg = &session->messages[session->count];
  memset(msg, 0, sizeof(*msg));
  msg->role = role;

  st = agent_message_reserve_parts(msg, part_count);
  if (st != AGENT_OK) {
    agent_message_destroy(msg);
    return st;
  }
  for (size_t i = 0; i < part_count; i++) {
    st = agent_part_copy(&msg->parts[msg->part_count], &parts[i]);
    if (st != AGENT_OK) {
      agent_message_destroy(msg);
      return st;
    }
    msg->part_count += 1;
  }

  st = agent_message_derive_text_content(msg);
  if (st != AGENT_OK) {
    agent_message_destroy(msg);
    return st;
  }

  session->count += 1;
  return AGENT_OK;
}

size_t agent_session_message_part_count(const agent_session_t* session, size_t message_index) {
  if (!session || message_index >= session->count) {
    return 0;
  }
  return session->messages[message_index].part_count;
}

agent_status_t agent_session_get_message_part(
  const agent_session_t* session,
  size_t message_index,
  size_t part_index,
  agent_content_part_view_t* out_view
) {
  if (!session || !out_view) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (message_index >= session->count) {
    return AGENT_ERR_BOUNDS;
  }
  const agent_message_t* msg = &session->messages[message_index];
  if (part_index >= msg->part_count) {
    return AGENT_ERR_BOUNDS;
  }
  const agent_part_t* p = &msg->parts[part_index];
  out_view->type = p->type;
  out_view->text = p->text ? p->text : "";
  out_view->text_len = p->text_len;
  out_view->url = p->url ? p->url : "";
  out_view->url_len = p->url_len;
  out_view->bytes = p->bytes;
  out_view->bytes_len = p->bytes_len;
  out_view->mime = p->mime ? p->mime : "";
  out_view->mime_len = p->mime_len;
  return AGENT_OK;
}
