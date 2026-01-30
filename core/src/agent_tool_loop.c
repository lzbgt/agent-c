#include "agent/tool_loop.h"

#include "agent_alloc.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char* kCompactionSummaryName = "__agent_compaction_summary__";

const char* agent_tool_loop_compaction_summary_name(void) {
  return kCompactionSummaryName;
}

typedef struct tl_tool_call {
  agent_string_t id;
  agent_string_t name;
  agent_string_t arguments_json;
} tl_tool_call_t;

typedef struct tl_message {
  agent_role_t role;
  agent_string_t content;
  agent_string_t name;
  agent_string_t tool_call_id;
  tl_tool_call_t* tool_calls;
  size_t tool_call_count;
} tl_message_t;

typedef struct tl_msg_list {
  tl_message_t* msgs;
  size_t count;
  size_t cap;
} tl_msg_list_t;

typedef struct tl_buf {
  char* data;
  size_t len;
  size_t cap;
} tl_buf_t;

static void tl_buf_free(tl_buf_t* b) {
  if (!b) return;
  if (b->data) agent_free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static agent_status_t tl_buf_reserve(tl_buf_t* b, size_t need) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  if (need <= b->cap) return AGENT_OK;
  size_t new_cap = b->cap == 0 ? 256 : b->cap;
  while (new_cap < need) new_cap *= 2;
  char* p = (char*)agent_malloc(new_cap);
  if (!p) return AGENT_ERR_OOM;
  if (b->data && b->len) memcpy(p, b->data, b->len);
  if (b->data) agent_free(b->data);
  b->data = p;
  b->cap = new_cap;
  return AGENT_OK;
}

static agent_status_t tl_buf_append_bytes(tl_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = tl_buf_reserve(b, b->len + n + 1);
  if (st != AGENT_OK) return st;
  if (n) memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return AGENT_OK;
}

static agent_status_t tl_buf_append_cstr(tl_buf_t* b, const char* s) {
  if (!s) s = "";
  return tl_buf_append_bytes(b, s, strlen(s));
}

static agent_status_t tl_buf_append_char(tl_buf_t* b, char c) {
  return tl_buf_append_bytes(b, &c, 1);
}

static agent_status_t tl_buf_append_u64(tl_buf_t* b, unsigned long long x) {
  char tmp[64];
  const int n = snprintf(tmp, sizeof(tmp), "%llu", x);
  if (n <= 0) return AGENT_ERR_INTERNAL;
  return tl_buf_append_bytes(b, tmp, (size_t)n);
}

static agent_status_t tl_buf_append_i64(tl_buf_t* b, long long x) {
  char tmp[64];
  const int n = snprintf(tmp, sizeof(tmp), "%lld", x);
  if (n <= 0) return AGENT_ERR_INTERNAL;
  return tl_buf_append_bytes(b, tmp, (size_t)n);
}

static agent_status_t tl_json_escape_into(tl_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '\\': {
        agent_status_t st = tl_buf_append_cstr(b, "\\\\");
        if (st != AGENT_OK) return st;
        break;
      }
      case '"': {
        agent_status_t st = tl_buf_append_cstr(b, "\\\"");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\n': {
        agent_status_t st = tl_buf_append_cstr(b, "\\n");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\r': {
        agent_status_t st = tl_buf_append_cstr(b, "\\r");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\t': {
        agent_status_t st = tl_buf_append_cstr(b, "\\t");
        if (st != AGENT_OK) return st;
        break;
      }
      default: {
        if (c < 0x20) {
          char tmp[8];
          const int m = snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)c);
          if (m <= 0) return AGENT_ERR_INTERNAL;
          agent_status_t st = tl_buf_append_bytes(b, tmp, (size_t)m);
          if (st != AGENT_OK) return st;
        } else {
          agent_status_t st = tl_buf_append_char(b, (char)c);
          if (st != AGENT_OK) return st;
        }
      }
    }
  }
  return AGENT_OK;
}

static agent_status_t tl_json_append_string_field(
  tl_buf_t* b,
  const char* key,
  const char* value,
  size_t value_len,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = AGENT_OK;
  st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_json_escape_into(b, key, strlen(key)); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":\""); if (st != AGENT_OK) return st;
  if (!value) { value = ""; value_len = 0; }
  st = tl_json_escape_into(b, value, value_len); if (st != AGENT_OK) return st;
  st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  return AGENT_OK;
}

static agent_status_t tl_json_append_u64_field(
  tl_buf_t* b,
  const char* key,
  unsigned long long value,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = AGENT_OK;
  st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_json_escape_into(b, key, strlen(key)); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":"); if (st != AGENT_OK) return st;
  st = tl_buf_append_u64(b, value); if (st != AGENT_OK) return st;
  return AGENT_OK;
}

static agent_status_t tl_json_append_i64_field(
  tl_buf_t* b,
  const char* key,
  long long value,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = AGENT_OK;
  st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_json_escape_into(b, key, strlen(key)); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":"); if (st != AGENT_OK) return st;
  st = tl_buf_append_i64(b, value); if (st != AGENT_OK) return st;
  return AGENT_OK;
}

static void tl_tool_call_destroy(tl_tool_call_t* c) {
  if (!c) return;
  agent_string_free(&c->id);
  agent_string_free(&c->name);
  agent_string_free(&c->arguments_json);
  memset(c, 0, sizeof(*c));
}

static void tl_message_destroy(tl_message_t* m) {
  if (!m) return;
  agent_string_free(&m->content);
  agent_string_free(&m->name);
  agent_string_free(&m->tool_call_id);
  if (m->tool_calls) {
    for (size_t i = 0; i < m->tool_call_count; i++) {
      tl_tool_call_destroy(&m->tool_calls[i]);
    }
    agent_free(m->tool_calls);
  }
  memset(m, 0, sizeof(*m));
}

static agent_status_t tl_list_reserve(tl_msg_list_t* l, size_t need) {
  if (!l) return AGENT_ERR_INVALID_ARGUMENT;
  if (need <= l->cap) return AGENT_OK;
  size_t new_cap = l->cap == 0 ? 16 : l->cap;
  while (new_cap < need) new_cap *= 2;
  tl_message_t* p = (tl_message_t*)agent_malloc(new_cap * sizeof(tl_message_t));
  if (!p) return AGENT_ERR_OOM;
  memset(p, 0, new_cap * sizeof(tl_message_t));
  for (size_t i = 0; i < l->count; i++) {
    p[i] = l->msgs[i];
  }
  if (l->msgs) agent_free(l->msgs);
  l->msgs = p;
  l->cap = new_cap;
  return AGENT_OK;
}

static agent_status_t tl_list_push_message_copy(
  tl_msg_list_t* l,
  agent_role_t role,
  const char* content,
  const char* name_or_null,
  const char* tool_call_id_or_null
) {
  if (!l || !content) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = tl_list_reserve(l, l->count + 1);
  if (st != AGENT_OK) return st;
  tl_message_t* m = &l->msgs[l->count];
  memset(m, 0, sizeof(*m));
  m->role = role;
  st = agent_string_set_copy(&m->content, content, strlen(content));
  if (st != AGENT_OK) { tl_message_destroy(m); return st; }
  if (name_or_null && name_or_null[0]) {
    st = agent_string_set_copy(&m->name, name_or_null, strlen(name_or_null));
    if (st != AGENT_OK) { tl_message_destroy(m); return st; }
  }
  if (tool_call_id_or_null && tool_call_id_or_null[0]) {
    st = agent_string_set_copy(&m->tool_call_id, tool_call_id_or_null, strlen(tool_call_id_or_null));
    if (st != AGENT_OK) { tl_message_destroy(m); return st; }
  }
  l->count += 1;
  return AGENT_OK;
}

static agent_status_t tl_list_push_assistant_with_tool_calls(
  tl_msg_list_t* l,
  const agent_tool_provider_response_t* resp
) {
  if (!l || !resp) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = tl_list_reserve(l, l->count + 1);
  if (st != AGENT_OK) return st;
  tl_message_t* m = &l->msgs[l->count];
  memset(m, 0, sizeof(*m));
  m->role = AGENT_ROLE_ASSISTANT;
  st = agent_string_set_copy(&m->content, resp->assistant_content.data ? resp->assistant_content.data : "", resp->assistant_content.len);
  if (st != AGENT_OK) { tl_message_destroy(m); return st; }
  if (resp->tool_call_count > 0) {
    m->tool_calls = (tl_tool_call_t*)agent_malloc(resp->tool_call_count * sizeof(tl_tool_call_t));
    if (!m->tool_calls) { tl_message_destroy(m); return AGENT_ERR_OOM; }
    memset(m->tool_calls, 0, resp->tool_call_count * sizeof(tl_tool_call_t));
    m->tool_call_count = resp->tool_call_count;
    for (size_t i = 0; i < resp->tool_call_count; i++) {
      const agent_tool_call_t* src = &resp->tool_calls[i];
      tl_tool_call_t* dst = &m->tool_calls[i];
      st = agent_string_set_copy(&dst->id, src->id.data ? src->id.data : "", src->id.len);
      if (st != AGENT_OK) { tl_message_destroy(m); return st; }
      st = agent_string_set_copy(&dst->name, src->name.data ? src->name.data : "", src->name.len);
      if (st != AGENT_OK) { tl_message_destroy(m); return st; }
      st = agent_string_set_copy(&dst->arguments_json, src->arguments_json.data ? src->arguments_json.data : "", src->arguments_json.len);
      if (st != AGENT_OK) { tl_message_destroy(m); return st; }
    }
  }
  l->count += 1;
  return AGENT_OK;
}

static size_t tl_estimated_chars_message(const tl_message_t* m) {
  if (!m) return 0;
  size_t sum = 12;
  sum += m->content.len;
  sum += m->name.len;
  sum += m->tool_call_id.len;
  for (size_t i = 0; i < m->tool_call_count; i++) {
    sum += 20;
    sum += m->tool_calls[i].id.len;
    sum += m->tool_calls[i].name.len;
    sum += m->tool_calls[i].arguments_json.len;
  }
  return sum;
}

static size_t tl_estimated_chars_list(const tl_msg_list_t* l) {
  if (!l) return 0;
  size_t total = 0;
  for (size_t i = 0; i < l->count; i++) {
    total += tl_estimated_chars_message(&l->msgs[i]);
  }
  return total;
}

static uint8_t tl_message_is_compaction_summary_system(const tl_message_t* m) {
  if (!m) return 0;
  if (m->role != AGENT_ROLE_SYSTEM) return 0;
  if (!m->name.data || m->name.len == 0) return 0;
  return (strcmp(m->name.data, kCompactionSummaryName) == 0) ? 1 : 0;
}

static size_t tl_pinned_system_prefix_count(const tl_msg_list_t* l) {
  if (!l) return 0;
  size_t pinned = 0;
  for (size_t i = 0; i < l->count; i++) {
    const tl_message_t* m = &l->msgs[i];
    if (m->role != AGENT_ROLE_SYSTEM) break;
    if (tl_message_is_compaction_summary_system(m)) break;
    pinned++;
  }
  // Preserve the first non-summary system message if it exists.
  if (pinned == 0 && l->count > 0 && l->msgs[0].role == AGENT_ROLE_SYSTEM && !tl_message_is_compaction_summary_system(&l->msgs[0])) {
    pinned = 1;
  }
  return pinned;
}

static const char* tl_role_string(agent_role_t role) {
  switch (role) {
    case AGENT_ROLE_SYSTEM: return "system";
    case AGENT_ROLE_USER: return "user";
    case AGENT_ROLE_ASSISTANT: return "assistant";
    case AGENT_ROLE_TOOL: return "tool";
    default: return "unknown";
  }
}

static void tl_trim_in_place(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  size_t i = 0;
  while (i < n && isspace((unsigned char)s[i])) i++;
  size_t j = n;
  while (j > i && isspace((unsigned char)s[j - 1])) j--;
  if (i > 0 || j < n) {
    const size_t out_n = j - i;
    memmove(s, s + i, out_n);
    s[out_n] = '\0';
  }
}

static agent_status_t tl_build_compaction_summary(
  const tl_msg_list_t* l,
  size_t drop_begin,
  size_t drop_end,
  const agent_tool_loop_options_t* opt,
  agent_string_t* out_summary
) {
  if (!l || !opt || !out_summary) return AGENT_ERR_INVALID_ARGUMENT;
  agent_string_free(out_summary);
  if (drop_end <= drop_begin || drop_end > l->count) return AGENT_OK;

  const size_t dropped = drop_end - drop_begin;
  tl_buf_t b = {0};
  agent_status_t st = AGENT_OK;

  st = tl_buf_append_cstr(&b, "Previous conversation truncated (");
  if (st != AGENT_OK) goto done;
  st = tl_buf_append_u64(&b, (unsigned long long)dropped);
  if (st != AGENT_OK) goto done;
  st = tl_buf_append_cstr(&b, " earlier messages omitted) to stay within the model context window.\n");
  if (st != AGENT_OK) goto done;

  const size_t preview_n = opt->summary_preview_items == 0 ? 0 : ((opt->summary_preview_items < dropped) ? opt->summary_preview_items : dropped);
  const size_t preview_start = drop_end - preview_n;
  for (size_t i = 0; i < preview_n; i++) {
    const size_t idx = preview_start + i;
    if (idx >= l->count) break;
    const tl_message_t* m = &l->msgs[idx];

    // Extract a one-line snippet.
    size_t snippet_len = m->content.len;
    const char* content = m->content.data ? m->content.data : "";
    for (size_t k = 0; k < m->content.len; k++) {
      if (content[k] == '\n' || content[k] == '\r') { snippet_len = k; break; }
    }
    const size_t max_snip = opt->summary_snippet_chars;
    uint8_t truncated = 0;
    size_t want = snippet_len;
    if (max_snip > 0 && want > max_snip) {
      want = max_snip;
      truncated = 1;
    }

    // Allocate a small slack so we can append an ellipsis when truncated.
    char* tmp = (char*)agent_malloc(want + 8);
    if (!tmp) { st = AGENT_ERR_OOM; goto done; }
    if (want) memcpy(tmp, content, want);
    tmp[want] = '\0';
    tl_trim_in_place(tmp);
    if (truncated) {
      const size_t cur = strlen(tmp);
      // UTF-8 ellipsis U+2026: 0xE2 0x80 0xA6
      if (cur + 3 + 1 < want + 8) {
        tmp[cur] = (char)0xE2;
        tmp[cur + 1] = (char)0x80;
        tmp[cur + 2] = (char)0xA6;
        tmp[cur + 3] = '\0';
      }
    }

    st = tl_buf_append_u64(&b, (unsigned long long)(i + 1));
    if (st != AGENT_OK) { agent_free(tmp); goto done; }
    st = tl_buf_append_cstr(&b, ". ");
    if (st != AGENT_OK) { agent_free(tmp); goto done; }
    st = tl_buf_append_cstr(&b, tl_role_string(m->role));
    if (st != AGENT_OK) { agent_free(tmp); goto done; }
    if (tmp[0]) {
      st = tl_buf_append_cstr(&b, ": ");
      if (st != AGENT_OK) { agent_free(tmp); goto done; }
      st = tl_buf_append_cstr(&b, tmp);
      if (st != AGENT_OK) { agent_free(tmp); goto done; }
    }
    agent_free(tmp);
    st = tl_buf_append_char(&b, '\n');
    if (st != AGENT_OK) goto done;
  }

  // Drop trailing newline.
  if (b.len > 0 && b.data && b.data[b.len - 1] == '\n') {
    b.data[b.len - 1] = '\0';
    b.len -= 1;
  }
  if (opt->summary_max_chars > 0 && b.len > opt->summary_max_chars) {
    b.len = opt->summary_max_chars;
    if (b.data) b.data[b.len ? (b.len - 1) : 0] = '\0';
  }
  st = agent_string_set_copy(out_summary, b.data ? b.data : "", b.len);

done:
  tl_buf_free(&b);
  return st;
}

typedef struct tl_compaction_report {
  size_t before_chars;
  size_t after_chars;
  size_t pinned_system_messages;
  size_t dropped_messages;
  uint8_t inserted_summary;
  agent_string_t summary;
} tl_compaction_report_t;

static void tl_compaction_report_destroy(tl_compaction_report_t* r) {
  if (!r) return;
  agent_string_free(&r->summary);
  memset(r, 0, sizeof(*r));
}

static agent_status_t tl_compact_in_place(
  tl_msg_list_t* l,
  size_t max_chars_budget,
  const agent_tool_loop_options_t* opt,
  tl_compaction_report_t* out_rep
) {
  if (!l || !opt || max_chars_budget == 0) return AGENT_ERR_INVALID_ARGUMENT;

  tl_compaction_report_t rep = {0};
  rep.before_chars = tl_estimated_chars_list(l);
  rep.after_chars = rep.before_chars;
  if (rep.before_chars <= max_chars_budget) {
    if (out_rep) *out_rep = rep;
    return AGENT_OK;
  }

  const size_t keep_last = opt->keep_last_messages == 0 ? 16 : opt->keep_last_messages;
  rep.pinned_system_messages = tl_pinned_system_prefix_count(l);
  const size_t pinned = rep.pinned_system_messages;
  const size_t n = l->count;

  size_t suffix_start = (n > keep_last) ? (n - keep_last) : pinned;
  if (suffix_start < pinned) suffix_start = pinned;

  // Avoid keeping a suffix that starts with a tool message (would break OpenAI tool-call consistency).
  if (suffix_start < n && l->msgs[suffix_start].role == AGENT_ROLE_TOOL) {
    size_t s = suffix_start;
    while (s > pinned && l->msgs[s - 1].role == AGENT_ROLE_TOOL) s--;
    // Now s points to the first tool in a tool-result run; include its preceding assistant tool-call message if present.
    if (s > pinned && l->msgs[s - 1].role == AGENT_ROLE_ASSISTANT) {
      suffix_start = s - 1;
    } else {
      suffix_start = s;
    }
  }

  const size_t drop_begin = pinned;
  const size_t drop_end = (suffix_start < n) ? suffix_start : n;
  if (drop_end <= drop_begin) {
    if (out_rep) *out_rep = rep;
    return AGENT_OK;
  }

  rep.dropped_messages = drop_end - drop_begin;

  agent_string_t summary = {0};
  if (opt->insert_compaction_summary) {
    agent_status_t st = tl_build_compaction_summary(l, drop_begin, drop_end, opt, &summary);
    if (st != AGENT_OK) {
      agent_string_free(&summary);
      return st;
    }
    if (summary.data && summary.data[0]) {
      rep.inserted_summary = 1;
      rep.summary = summary;
    } else {
      agent_string_free(&summary);
    }
  }

  const size_t new_n = pinned + (rep.inserted_summary ? 1u : 0u) + (n - drop_end);
  tl_message_t* out = (tl_message_t*)agent_malloc(new_n * sizeof(tl_message_t));
  if (!out) {
    tl_compaction_report_destroy(&rep);
    return AGENT_ERR_OOM;
  }
  memset(out, 0, new_n * sizeof(tl_message_t));

  size_t out_i = 0;
  // Move prefix.
  for (size_t i = 0; i < pinned; i++) {
    out[out_i++] = l->msgs[i];
    memset(&l->msgs[i], 0, sizeof(tl_message_t));
  }

  if (rep.inserted_summary) {
    tl_message_t* sm = &out[out_i++];
    memset(sm, 0, sizeof(*sm));
    sm->role = AGENT_ROLE_SYSTEM;
    (void)agent_string_set_copy(&sm->name, kCompactionSummaryName, strlen(kCompactionSummaryName));
    sm->content = rep.summary;
    memset(&rep.summary, 0, sizeof(rep.summary));
  }

  // Drop range: destroy.
  for (size_t i = drop_begin; i < drop_end; i++) {
    tl_message_destroy(&l->msgs[i]);
  }

  // Move suffix.
  for (size_t i = drop_end; i < n; i++) {
    out[out_i++] = l->msgs[i];
    memset(&l->msgs[i], 0, sizeof(tl_message_t));
  }

  // Destroy remaining old messages (holes) and replace array.
  for (size_t i = 0; i < l->count; i++) {
    tl_message_destroy(&l->msgs[i]);
  }
  if (l->msgs) agent_free(l->msgs);
  l->msgs = out;
  l->count = new_n;
  l->cap = new_n;

  rep.after_chars = tl_estimated_chars_list(l);
  if (out_rep) *out_rep = rep;
  return AGENT_OK;
}

static void tl_emit_event(const agent_tool_loop_hooks_t* hooks, const char* type, const char* data_json) {
  if (!hooks || !hooks->on_event || !type) return;
  hooks->on_event(hooks->on_event_ctx, type, data_json ? data_json : "");
}

static uint8_t tl_should_cancel(const agent_tool_loop_hooks_t* hooks) {
  if (!hooks || !hooks->should_cancel) return 0;
  return hooks->should_cancel(hooks->should_cancel_ctx) ? 1 : 0;
}

static agent_status_t tl_cap_output_default(
  void* ctx,
  const char* tool_out,
  size_t tool_out_len,
  size_t max_chars,
  agent_string_t* out_capped,
  uint8_t* out_truncated
) {
  (void)ctx;
  if (out_truncated) *out_truncated = 0;
  if (!out_capped) return AGENT_ERR_INVALID_ARGUMENT;
  agent_string_free(out_capped);
  if (!tool_out) tool_out = "";
  if (max_chars == 0 || tool_out_len <= max_chars) {
    return agent_string_set_copy(out_capped, tool_out, tool_out_len);
  }
  if (out_truncated) *out_truncated = 1;
  return agent_string_set_copy(out_capped, tool_out, max_chars);
}

static agent_status_t tl_cap_output(
  agent_tool_loop_cap_output_fn fn,
  void* fn_ctx,
  const char* tool_out,
  size_t tool_out_len,
  size_t max_chars,
  agent_string_t* out_capped,
  uint8_t* out_truncated
) {
  if (!fn) {
    return tl_cap_output_default(NULL, tool_out, tool_out_len, max_chars, out_capped, out_truncated);
  }
  return fn(fn_ctx, tool_out, tool_out_len, max_chars, out_capped, out_truncated);
}

static agent_status_t tl_summarize_output(
  const agent_tool_loop_hooks_t* hooks,
  const char* tool_out,
  size_t tool_out_len,
  agent_string_t* out_summary_json
) {
  if (!out_summary_json) return AGENT_ERR_INVALID_ARGUMENT;
  agent_string_free(out_summary_json);
  if (!hooks || !hooks->summarize_tool_output) {
    return AGENT_OK;
  }
  return hooks->summarize_tool_output(hooks->summarize_tool_output_ctx, tool_out, tool_out_len, out_summary_json);
}

static agent_status_t tl_append_tool_record(
  agent_tool_loop_result_t* out,
  const char* tool_name,
  const char* tool_call_id,
  const char* args_json,
  const agent_string_t* result_string,
  const agent_string_t* result_for_prompt,
  uint8_t truncated_for_prompt
) {
  if (!out || !tool_name || !args_json || !result_string || !result_for_prompt) return AGENT_ERR_INVALID_ARGUMENT;

  const size_t n = out->tool_record_count;
  agent_tool_record_t* p = (agent_tool_record_t*)agent_malloc((n + 1) * sizeof(agent_tool_record_t));
  if (!p) return AGENT_ERR_OOM;
  memset(p, 0, (n + 1) * sizeof(agent_tool_record_t));

  for (size_t i = 0; i < n; i++) {
    p[i] = out->tool_records[i];
  }
  if (out->tool_records) agent_free(out->tool_records);
  out->tool_records = p;
  out->tool_record_count = n + 1;

  agent_tool_record_t* r = &out->tool_records[n];
  agent_status_t st = AGENT_OK;
  st = agent_string_set_copy(&r->tool_name, tool_name, strlen(tool_name));
  if (st != AGENT_OK) return st;
  if (tool_call_id) {
    st = agent_string_set_copy(&r->tool_call_id, tool_call_id, strlen(tool_call_id));
    if (st != AGENT_OK) return st;
  }
  st = agent_string_set_copy(&r->arguments_json, args_json, strlen(args_json));
  if (st != AGENT_OK) return st;
  st = agent_string_set_copy(&r->result_string, result_string->data ? result_string->data : "", result_string->len);
  if (st != AGENT_OK) return st;
  st = agent_string_set_copy(&r->result_string_for_prompt, result_for_prompt->data ? result_for_prompt->data : "", result_for_prompt->len);
  if (st != AGENT_OK) return st;
  r->result_truncated_for_prompt = truncated_for_prompt ? 1 : 0;
  return AGENT_OK;
}

void agent_tool_loop_result_free(agent_tool_loop_result_t* r) {
  if (!r) return;
  agent_string_free(&r->final_assistant_text);
  if (r->tool_records) {
    for (size_t i = 0; i < r->tool_record_count; i++) {
      agent_tool_record_t* tr = &r->tool_records[i];
      agent_string_free(&tr->tool_name);
      agent_string_free(&tr->tool_call_id);
      agent_string_free(&tr->arguments_json);
      agent_string_free(&tr->result_string);
      agent_string_free(&tr->result_string_for_prompt);
    }
    agent_free(r->tool_records);
  }
  agent_string_free(&r->error_message);
  memset(r, 0, sizeof(*r));
}

static agent_status_t tl_build_message_views(
  const tl_msg_list_t* l,
  agent_chat_message_view_t** out_views,
  agent_chat_tool_call_view_t** out_call_views,
  size_t* out_total_call_views
) {
  if (!l || !out_views || !out_call_views || !out_total_call_views) return AGENT_ERR_INVALID_ARGUMENT;
  *out_views = NULL;
  *out_call_views = NULL;
  *out_total_call_views = 0;

  agent_chat_message_view_t* views = NULL;
  if (l->count > 0) {
    views = (agent_chat_message_view_t*)agent_malloc(l->count * sizeof(agent_chat_message_view_t));
    if (!views) return AGENT_ERR_OOM;
    memset(views, 0, l->count * sizeof(agent_chat_message_view_t));
  }

  size_t total_calls = 0;
  for (size_t i = 0; i < l->count; i++) {
    total_calls += l->msgs[i].tool_call_count;
  }
  agent_chat_tool_call_view_t* calls = NULL;
  if (total_calls > 0) {
    calls = (agent_chat_tool_call_view_t*)agent_malloc(total_calls * sizeof(agent_chat_tool_call_view_t));
    if (!calls) { if (views) agent_free(views); return AGENT_ERR_OOM; }
    memset(calls, 0, total_calls * sizeof(agent_chat_tool_call_view_t));
  }

  size_t call_off = 0;
  for (size_t i = 0; i < l->count; i++) {
    const tl_message_t* m = &l->msgs[i];
    agent_chat_message_view_t* v = &views[i];
    v->role = m->role;
    v->content = m->content.data ? m->content.data : "";
    v->content_len = m->content.len;
    v->name = m->name.data;
    v->tool_call_id = m->tool_call_id.data;
    v->tool_calls = NULL;
    v->tool_call_count = 0;
    if (m->tool_call_count > 0) {
      v->tool_calls = &calls[call_off];
      v->tool_call_count = m->tool_call_count;
      for (size_t j = 0; j < m->tool_call_count; j++) {
        calls[call_off + j].id = m->tool_calls[j].id.data;
        calls[call_off + j].name = m->tool_calls[j].name.data;
        calls[call_off + j].arguments_json = m->tool_calls[j].arguments_json.data;
      }
      call_off += m->tool_call_count;
    }
  }

  *out_views = views;
  *out_call_views = calls;
  *out_total_call_views = total_calls;
  return AGENT_OK;
}

agent_status_t agent_tool_loop_run(
  const agent_tool_provider_t* provider,
  const agent_tool_registry_t* tools,
  const agent_tool_executor_t* executor,
  const agent_session_t* seed_session,
  const char* user_prompt,
  const agent_tool_loop_options_t* options,
  const agent_tool_loop_hooks_t* hooks,
  agent_tool_loop_result_t* out_result
) {
  if (!provider || !provider->generate || !tools || !executor || !executor->execute || !seed_session || !user_prompt || !options || !options->model || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  agent_tool_loop_result_t res = {0};
  agent_status_t status = AGENT_OK;
  agent_tool_provider_response_t presp = {0};
  const size_t max_chars = options->max_chars == 0 ? 20000 : options->max_chars;
  const size_t keep_last = options->keep_last_messages == 0 ? 16 : options->keep_last_messages;
  const size_t max_capture = options->max_capture_chars == 0 ? (256 * 1024) : options->max_capture_chars;
  const size_t max_tool_result_chars = options->max_tool_result_chars;
  const size_t max_retries = options->max_context_too_long_retries == 0 ? 2 : options->max_context_too_long_retries;

  uint64_t epoch = 0;
  {
    tl_buf_t d = {0};
    uint8_t first = 1;
    (void)tl_buf_append_char(&d, '{');
    (void)tl_json_append_u64_field(&d, "epoch", (unsigned long long)epoch, &first);
    (void)tl_json_append_string_field(&d, "model", options->model, strlen(options->model), &first);
    (void)tl_buf_append_char(&d, '}');
    tl_emit_event(hooks, "start", d.data ? d.data : "{}");
    tl_buf_free(&d);
  }

  tl_msg_list_t messages = {0};

  // Seed transcript from the session.
  const size_t n_seed = agent_session_message_count(seed_session);
  for (size_t i = 0; i < n_seed; i++) {
    agent_message_view_t v = {0};
    if (agent_session_get_message(seed_session, i, &v) != AGENT_OK) continue;
    // Tool loop transcript is host-driven; carry through all roles as plain content.
    agent_status_t st = tl_list_push_message_copy(&messages, v.role, v.content ? v.content : "", NULL, NULL);
    if (st != AGENT_OK) {
      status = st;
      goto cleanup;
    }
  }

  // Initial compaction with budget reserved for the new user prompt.
  const size_t prompt_len = strlen(user_prompt);
  const size_t budget_for_history = max_chars > prompt_len ? (max_chars - prompt_len) : 1;
  tl_compaction_report_t crep = {0};
  agent_status_t stc = tl_compact_in_place(&messages, budget_for_history, options, &crep);
  if (stc != AGENT_OK) {
    tl_compaction_report_destroy(&crep);
    status = stc;
    goto cleanup;
  }
  if (crep.dropped_messages > 0) {
    tl_buf_t d = {0};
    uint8_t first = 1;
    (void)tl_buf_append_char(&d, '{');
    (void)tl_json_append_u64_field(&d, "epoch", (unsigned long long)epoch, &first);
    (void)tl_json_append_u64_field(&d, "step", 0, &first);
    (void)tl_json_append_u64_field(&d, "before_chars", (unsigned long long)(crep.before_chars + prompt_len), &first);
    (void)tl_json_append_u64_field(&d, "after_chars", (unsigned long long)(crep.after_chars + prompt_len), &first);
    (void)tl_json_append_u64_field(&d, "max_chars", (unsigned long long)max_chars, &first);
    (void)tl_json_append_u64_field(&d, "keep_last_messages", (unsigned long long)keep_last, &first);
    (void)tl_json_append_u64_field(&d, "pinned_system_messages", (unsigned long long)crep.pinned_system_messages, &first);
    (void)tl_json_append_u64_field(&d, "dropped_messages", (unsigned long long)crep.dropped_messages, &first);
    (void)tl_json_append_u64_field(&d, "inserted_summary", (unsigned long long)crep.inserted_summary, &first);
    if (options->verbose_events && crep.inserted_summary && crep.summary.data) {
      // Cap summary content for event capture.
      agent_string_t capped = {0};
      uint8_t trunc = 0;
      (void)tl_cap_output(hooks ? hooks->cap_tool_output_for_event : NULL, hooks ? hooks->cap_tool_output_for_event_ctx : NULL,
                          crep.summary.data, crep.summary.len, max_capture, &capped, &trunc);
      (void)tl_json_append_string_field(&d, "summary", capped.data ? capped.data : "", capped.len, &first);
      (void)tl_json_append_u64_field(&d, "summary_truncated", (unsigned long long)trunc, &first);
      agent_string_free(&capped);
    }
    (void)tl_buf_append_char(&d, '}');
    tl_emit_event(hooks, "compaction", d.data ? d.data : "{}");
    tl_buf_free(&d);
    epoch++;
  }
  tl_compaction_report_destroy(&crep);

  // Append the new user prompt.
  agent_status_t st = tl_list_push_message_copy(&messages, AGENT_ROLE_USER, user_prompt, NULL, NULL);
  if (st != AGENT_OK) {
    status = st;
    goto cleanup;
  }

  for (size_t step = 0; options->max_steps == 0 || step < options->max_steps; step++) {
    res.steps_executed = step + 1;
    if (tl_should_cancel(hooks)) {
      (void)agent_string_set_copy(&res.error_message, "cancelled", strlen("cancelled"));
      tl_buf_t d = {0};
      uint8_t first = 1;
      (void)tl_buf_append_char(&d, '{');
      (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
      (void)tl_json_append_string_field(&d, "reason", "cancel_requested", strlen("cancel_requested"), &first);
      (void)tl_buf_append_char(&d, '}');
      tl_emit_event(hooks, "cancelled", d.data ? d.data : "{}");
      tl_buf_free(&d);
      status = AGENT_ERR_CANCELLED;
      goto cleanup;
    }

    // Regular compaction to max_chars.
    tl_compaction_report_t rep = {0};
    agent_status_t st_comp = tl_compact_in_place(&messages, max_chars, options, &rep);
    if (st_comp != AGENT_OK) {
      tl_compaction_report_destroy(&rep);
      status = st_comp;
      goto cleanup;
    }
    if (rep.dropped_messages > 0) {
      tl_buf_t d = {0};
      uint8_t first = 1;
      (void)tl_buf_append_char(&d, '{');
      (void)tl_json_append_u64_field(&d, "epoch", (unsigned long long)epoch, &first);
      (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
      (void)tl_json_append_u64_field(&d, "before_chars", (unsigned long long)rep.before_chars, &first);
      (void)tl_json_append_u64_field(&d, "after_chars", (unsigned long long)rep.after_chars, &first);
      (void)tl_json_append_u64_field(&d, "max_chars", (unsigned long long)max_chars, &first);
      (void)tl_json_append_u64_field(&d, "keep_last_messages", (unsigned long long)keep_last, &first);
      (void)tl_json_append_u64_field(&d, "pinned_system_messages", (unsigned long long)rep.pinned_system_messages, &first);
      (void)tl_json_append_u64_field(&d, "dropped_messages", (unsigned long long)rep.dropped_messages, &first);
      (void)tl_json_append_u64_field(&d, "inserted_summary", (unsigned long long)rep.inserted_summary, &first);
      if (options->verbose_events && rep.inserted_summary && rep.summary.data) {
        agent_string_t capped = {0};
        uint8_t trunc = 0;
        (void)tl_cap_output(hooks ? hooks->cap_tool_output_for_event : NULL, hooks ? hooks->cap_tool_output_for_event_ctx : NULL,
                            rep.summary.data, rep.summary.len, max_capture, &capped, &trunc);
        (void)tl_json_append_string_field(&d, "summary", capped.data ? capped.data : "", capped.len, &first);
        (void)tl_json_append_u64_field(&d, "summary_truncated", (unsigned long long)trunc, &first);
        agent_string_free(&capped);
      }
      (void)tl_json_append_u64_field(&d, "epoch_after", (unsigned long long)(epoch + 1), &first);
      (void)tl_buf_append_char(&d, '}');
      tl_emit_event(hooks, "compaction", d.data ? d.data : "{}");
      tl_buf_free(&d);
      epoch++;
    }
    tl_compaction_report_destroy(&rep);

    // Call provider (with context-too-long retries).
    agent_status_t stp = AGENT_OK;

    for (size_t retry = 0; retry <= max_retries; retry++) {
      agent_chat_message_view_t* views = NULL;
      agent_chat_tool_call_view_t* call_views = NULL;
      size_t total_call_views = 0;
      agent_status_t stv = tl_build_message_views(&messages, &views, &call_views, &total_call_views);
      if (stv != AGENT_OK) {
        if (views) agent_free(views);
        if (call_views) agent_free(call_views);
        agent_tool_provider_response_free(&presp);
        status = stv;
        goto cleanup;
      }

      agent_tool_provider_request_t preq = {0};
      preq.model = options->model;
      preq.messages = views;
      preq.message_count = messages.count;
      preq.tools = tools;
      preq.force_tool_or_null = (step == 0) ? options->force_tool_or_null : NULL;
      preq.step = step;
      preq.epoch = epoch;

      agent_tool_provider_response_free(&presp);
      stp = provider->generate(provider->ctx, &preq, &presp);

      if (views) agent_free(views);
      if (call_views) agent_free(call_views);
      (void)total_call_views;

      if (stp == AGENT_ERR_CONTEXT_TOO_LONG && retry < max_retries) {
        // Emit a retry event.
        tl_buf_t d = {0};
        uint8_t first = 1;
        (void)tl_buf_append_char(&d, '{');
        (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
        (void)tl_json_append_u64_field(&d, "epoch", (unsigned long long)epoch, &first);
        (void)tl_json_append_string_field(&d, "reason", "context_too_long_retry", strlen("context_too_long_retry"), &first);
        (void)tl_buf_append_char(&d, '}');
        tl_emit_event(hooks, "retry", d.data ? d.data : "{}");
        tl_buf_free(&d);

        // Shrink budget and compact.
        agent_tool_loop_options_t tighter = *options;
        const size_t cur = tighter.max_chars == 0 ? 20000 : tighter.max_chars;
        tighter.max_chars = (cur * 3) / 4;
        if (tighter.max_chars < 2000) tighter.max_chars = 2000;

        tl_compaction_report_t r2 = {0};
        agent_status_t st2 = tl_compact_in_place(&messages, tighter.max_chars, &tighter, &r2);
        if (st2 != AGENT_OK) {
          tl_compaction_report_destroy(&r2);
          break;
        }
        if (r2.dropped_messages > 0) {
          tl_buf_t cdata = {0};
          uint8_t f2 = 1;
          (void)tl_buf_append_char(&cdata, '{');
          (void)tl_json_append_u64_field(&cdata, "epoch", (unsigned long long)epoch, &f2);
          (void)tl_json_append_u64_field(&cdata, "step", (unsigned long long)step, &f2);
          (void)tl_json_append_u64_field(&cdata, "before_chars", (unsigned long long)r2.before_chars, &f2);
          (void)tl_json_append_u64_field(&cdata, "after_chars", (unsigned long long)r2.after_chars, &f2);
          (void)tl_json_append_u64_field(&cdata, "max_chars", (unsigned long long)tighter.max_chars, &f2);
          (void)tl_json_append_u64_field(&cdata, "keep_last_messages", (unsigned long long)(tighter.keep_last_messages == 0 ? 16 : tighter.keep_last_messages), &f2);
          (void)tl_json_append_u64_field(&cdata, "pinned_system_messages", (unsigned long long)r2.pinned_system_messages, &f2);
          (void)tl_json_append_u64_field(&cdata, "dropped_messages", (unsigned long long)r2.dropped_messages, &f2);
          (void)tl_json_append_u64_field(&cdata, "inserted_summary", (unsigned long long)r2.inserted_summary, &f2);
          (void)tl_json_append_u64_field(&cdata, "epoch_after", (unsigned long long)(epoch + 1), &f2);
          (void)tl_buf_append_char(&cdata, '}');
          tl_emit_event(hooks, "compaction", cdata.data ? cdata.data : "{}");
          tl_buf_free(&cdata);
          epoch++;
        }
        tl_compaction_report_destroy(&r2);
        continue;
      }
      break;
    }

    if (stp != AGENT_OK) {
      if (presp.error_message.data && presp.error_message.data[0]) {
        (void)agent_string_set_copy(&res.error_message, presp.error_message.data, presp.error_message.len);
      } else {
        (void)agent_string_set_copy(&res.error_message, "provider error", strlen("provider error"));
      }
      tl_buf_t d = {0};
      uint8_t first = 1;
      (void)tl_buf_append_char(&d, '{');
      (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
      (void)tl_json_append_string_field(&d, "error", res.error_message.data ? res.error_message.data : "", res.error_message.len, &first);
      (void)tl_buf_append_char(&d, '}');
      tl_emit_event(hooks, "error", d.data ? d.data : "{}");
      tl_buf_free(&d);

      agent_tool_provider_response_free(&presp);
      status = stp;
      goto cleanup;
    }

    // Update final assistant text (best-effort).
    agent_string_free(&res.final_assistant_text);
    (void)agent_string_set_copy(&res.final_assistant_text,
                                presp.assistant_content.data ? presp.assistant_content.data : "",
                                presp.assistant_content.len);

    const uint8_t has_tool_calls = (presp.tool_call_count > 0) ? 1 : 0;
    if (has_tool_calls) res.saw_tool_call = 1;

    // Append assistant message (including tool call metadata).
    agent_status_t sta = tl_list_push_assistant_with_tool_calls(&messages, &presp);
    if (sta != AGENT_OK) {
      agent_tool_provider_response_free(&presp);
      status = sta;
      goto cleanup;
    }

    // assistant_message event
    {
      tl_buf_t d = {0};
      uint8_t first = 1;
      (void)tl_buf_append_char(&d, '{');
      (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
      (void)tl_json_append_string_field(&d, "assistant_content",
                                        presp.assistant_content.data ? presp.assistant_content.data : "",
                                        presp.assistant_content.len, &first);
      (void)tl_json_append_u64_field(&d, "has_tool_calls", (unsigned long long)has_tool_calls, &first);
      (void)tl_buf_append_char(&d, '}');
      tl_emit_event(hooks, "assistant_message", d.data ? d.data : "{}");
      tl_buf_free(&d);
    }

    if (!has_tool_calls) {
      tl_buf_t d = {0};
      uint8_t first = 1;
      (void)tl_buf_append_char(&d, '{');
      (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
      (void)tl_json_append_string_field(&d, "reason", "no tool calls", strlen("no tool calls"), &first);
      (void)tl_buf_append_char(&d, '}');
      tl_emit_event(hooks, "done", d.data ? d.data : "{}");
      tl_buf_free(&d);
      agent_tool_provider_response_free(&presp);
      break;
    }

    for (size_t i = 0; i < presp.tool_call_count; i++) {
      if (tl_should_cancel(hooks)) {
        (void)agent_string_set_copy(&res.error_message, "cancelled", strlen("cancelled"));
        tl_buf_t d = {0};
        uint8_t first = 1;
        (void)tl_buf_append_char(&d, '{');
        (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
        (void)tl_json_append_string_field(&d, "reason", "cancel_requested", strlen("cancel_requested"), &first);
        (void)tl_buf_append_char(&d, '}');
        tl_emit_event(hooks, "cancelled", d.data ? d.data : "{}");
        tl_buf_free(&d);
        agent_tool_provider_response_free(&presp);
        status = AGENT_ERR_CANCELLED;
        goto cleanup;
      }

      const agent_tool_call_t* call = &presp.tool_calls[i];
      const char* tool_name = call->name.data ? call->name.data : "";
      char tool_call_id_buf[64];
      const char* tool_call_id = call->id.data ? call->id.data : "";
      if (!tool_call_id[0]) {
        (void)snprintf(tool_call_id_buf, sizeof(tool_call_id_buf), "call_%llu_%llu",
                       (unsigned long long)step, (unsigned long long)i);
        tool_call_id = tool_call_id_buf;
      }
      const char* args_json = call->arguments_json.data ? call->arguments_json.data : "{}";

      // tool_call event
      {
        tl_buf_t d = {0};
        uint8_t first = 1;
        (void)tl_buf_append_char(&d, '{');
        (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
        (void)tl_json_append_string_field(&d, "tool_call_id", tool_call_id, strlen(tool_call_id), &first);
        (void)tl_json_append_string_field(&d, "tool_name", tool_name, strlen(tool_name), &first);
        if (options->verbose_events) {
          // cap args for event capture
          agent_string_t capped = {0};
          uint8_t trunc = 0;
          (void)tl_cap_output(hooks ? hooks->cap_tool_output_for_event : NULL, hooks ? hooks->cap_tool_output_for_event_ctx : NULL,
                              args_json, strlen(args_json), max_capture, &capped, &trunc);
          (void)tl_json_append_string_field(&d, "arguments_json", capped.data ? capped.data : "", capped.len, &first);
          agent_string_free(&capped);
        }
        (void)tl_buf_append_char(&d, '}');
        tl_emit_event(hooks, "tool_call", d.data ? d.data : "{}");
        tl_buf_free(&d);
      }

      agent_string_t tool_out = {0};
      agent_status_t st_tool = executor->execute(executor->ctx, tool_name, args_json, &tool_out);
      if (st_tool != AGENT_OK) {
        (void)agent_string_set_copy(&res.error_message, "tool execution failed", strlen("tool execution failed"));
        tl_buf_t d = {0};
        uint8_t first = 1;
        (void)tl_buf_append_char(&d, '{');
        (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
        (void)tl_json_append_string_field(&d, "tool_name", tool_name, strlen(tool_name), &first);
        (void)tl_json_append_i64_field(&d, "status", (long long)st_tool, &first);
        (void)tl_json_append_string_field(&d, "error", res.error_message.data ? res.error_message.data : "", res.error_message.len, &first);
        (void)tl_buf_append_char(&d, '}');
        tl_emit_event(hooks, "error", d.data ? d.data : "{}");
        tl_buf_free(&d);
        agent_string_free(&tool_out);
        agent_tool_provider_response_free(&presp);
        status = st_tool;
        goto cleanup;
      }

      // Cap tool output for prompt.
      agent_string_t tool_out_for_prompt = {0};
      uint8_t trunc_prompt = 0;
      (void)tl_cap_output(hooks ? hooks->cap_tool_output_for_prompt : NULL, hooks ? hooks->cap_tool_output_for_prompt_ctx : NULL,
                          tool_out.data ? tool_out.data : "", tool_out.len, max_tool_result_chars, &tool_out_for_prompt, &trunc_prompt);

      // Store tool record.
      (void)tl_append_tool_record(&res, tool_name, tool_call_id, args_json, &tool_out, &tool_out_for_prompt, trunc_prompt);

      // tool_result event
      {
        tl_buf_t d = {0};
        uint8_t first = 1;
        (void)tl_buf_append_char(&d, '{');
        (void)tl_json_append_u64_field(&d, "step", (unsigned long long)step, &first);
        (void)tl_json_append_string_field(&d, "tool_call_id", tool_call_id, strlen(tool_call_id), &first);
        (void)tl_json_append_string_field(&d, "tool_name", tool_name, strlen(tool_name), &first);
        (void)tl_json_append_i64_field(&d, "status", (long long)st_tool, &first);
        if (options->verbose_events) {
          agent_string_t capped = {0};
          uint8_t trunc = 0;
          (void)tl_cap_output(hooks ? hooks->cap_tool_output_for_event : NULL, hooks ? hooks->cap_tool_output_for_event_ctx : NULL,
                              tool_out.data ? tool_out.data : "", tool_out.len, max_capture, &capped, &trunc);
          (void)tl_json_append_string_field(&d, "content", capped.data ? capped.data : "", capped.len, &first);
          (void)tl_json_append_u64_field(&d, "content_truncated", (unsigned long long)trunc, &first);
          agent_string_free(&capped);
        } else {
          agent_string_t summary_json = {0};
          (void)tl_summarize_output(hooks, tool_out.data ? tool_out.data : "", tool_out.len, &summary_json);
          if (summary_json.data && summary_json.data[0]) {
            if (!first) (void)tl_buf_append_char(&d, ',');
            first = 0;
            (void)tl_buf_append_cstr(&d, "\"summary\":");
            (void)tl_buf_append_bytes(&d, summary_json.data, summary_json.len);
          }
          agent_string_free(&summary_json);
        }
        (void)tl_buf_append_char(&d, '}');
        tl_emit_event(hooks, "tool_result", d.data ? d.data : "{}");
        tl_buf_free(&d);
      }

      // Append tool result message to the transcript.
      agent_status_t st_tm = tl_list_push_message_copy(&messages, AGENT_ROLE_TOOL,
                                                       tool_out_for_prompt.data ? tool_out_for_prompt.data : "",
                                                       NULL,
                                                       tool_call_id);
      agent_string_free(&tool_out);
      agent_string_free(&tool_out_for_prompt);
      if (st_tm != AGENT_OK) {
        agent_tool_provider_response_free(&presp);
        status = st_tm;
        goto cleanup;
      }
    }

    agent_tool_provider_response_free(&presp);
  }

  if (options->require_tool_call && !res.saw_tool_call) {
    (void)agent_string_set_copy(&res.error_message, "no tool call occurred", strlen("no tool call occurred"));
    tl_buf_t d = {0};
    uint8_t first = 1;
    (void)tl_buf_append_char(&d, '{');
    (void)tl_json_append_string_field(&d, "error", res.error_message.data ? res.error_message.data : "", res.error_message.len, &first);
    (void)tl_buf_append_char(&d, '}');
    tl_emit_event(hooks, "error", d.data ? d.data : "{}");
    tl_buf_free(&d);
    status = AGENT_ERR_INTERNAL;
    goto cleanup;
  }

cleanup:
  agent_tool_provider_response_free(&presp);
  if (status == AGENT_OK) {
    agent_string_free(&res.error_message);
  }
  *out_result = res;

  for (size_t k = 0; k < messages.count; k++) tl_message_destroy(&messages.msgs[k]);
  if (messages.msgs) agent_free(messages.msgs);
  return status;
}
