#include "agent/um_eais_outbox_read.h"

#include "agent/edge_interop.h"

#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

static agent_status_t read_u64_best_effort(agent_cbor_reader_t* r, uint64_t* out) {
  // agentd encodes Json::Int64; for non-negative values that will be major0 (uint),
  // but be permissive and accept negative values by clamping to 0.
  int64_t x = 0;
  const agent_status_t st = agent_cbor_read_int(r, &x);
  if (st != AGENT_OK) return st;
  if (x < 0) x = 0;
  *out = (uint64_t)x;
  return AGENT_OK;
}

agent_status_t agent_um_eais_outbox_read_cbor_v0_1(
  const uint8_t* buf,
  size_t len,
  agent_um_eais_outbox_view_t* out,
  agent_um_eais_outbox_row_view_t* out_rows,
  size_t rows_cap
) {
  if (!buf || len == 0 || !out) return AGENT_ERR_INVALID_ARGUMENT;
  if (!out_rows && rows_cap != 0) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, buf, len);

  size_t top_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &top_pairs);
  if (st != AGENT_OK) return st;

  int have_ok = 0;
  int have_node_id = 0;
  int have_cursor_base = 0;
  int have_cursor_next = 0;
  int have_messages = 0;

  for (size_t i = 0; i < top_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return st;

    if (text_eq(k, "ok")) {
      int v = 0;
      st = agent_cbor_read_bool(&r, &v);
      if (st != AGENT_OK) return st;
      out->ok = v ? 1 : 0;
      have_ok = 1;
      continue;
    }

    if (text_eq(k, "node_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out->node_id = v;
      have_node_id = 1;
      continue;
    }

    if (text_eq(k, "cursor_base")) {
      uint64_t v = 0;
      st = read_u64_best_effort(&r, &v);
      if (st != AGENT_OK) return st;
      out->cursor_base = v;
      have_cursor_base = 1;
      continue;
    }

    if (text_eq(k, "cursor_next")) {
      uint64_t v = 0;
      st = read_u64_best_effort(&r, &v);
      if (st != AGENT_OK) return st;
      out->cursor_next = v;
      have_cursor_next = 1;
      continue;
    }

    if (text_eq(k, "messages")) {
      size_t n_rows = 0;
      st = agent_cbor_read_array_start(&r, &n_rows);
      if (st != AGENT_OK) return st;
      have_messages = 1;
      out->messages_total = n_rows;

      size_t parsed = 0;
      for (size_t ri = 0; ri < n_rows; ri++) {
        size_t row_pairs = 0;
        st = agent_cbor_read_map_start(&r, &row_pairs);
        if (st != AGENT_OK) return st;

        agent_um_eais_outbox_row_view_t row;
        memset(&row, 0, sizeof(row));

        agent_cbor_view_t msg_item = {0};
        int have_outbox_id = 0;
        int have_msg = 0;

        for (size_t pj = 0; pj < row_pairs; pj++) {
          agent_cbor_text_view_t rk;
          st = agent_cbor_read_text(&r, &rk);
          if (st != AGENT_OK) return st;

          if (text_eq(rk, "outbox_id")) {
            uint64_t v = 0;
            st = read_u64_best_effort(&r, &v);
            if (st != AGENT_OK) return st;
            row.outbox_id = v;
            have_outbox_id = 1;
            continue;
          }

          if (text_eq(rk, "ts_utc_ms")) {
            uint64_t v = 0;
            st = read_u64_best_effort(&r, &v);
            if (st != AGENT_OK) return st;
            row.ts_utc_ms = v;
            row.has_ts_utc_ms = 1;
            continue;
          }

          if (text_eq(rk, "msg")) {
            st = agent_cbor_read_item_view(&r, &msg_item);
            if (st != AGENT_OK) return st;
            have_msg = 1;
            continue;
          }

          // msg_raw / parse_error or unknown: skip.
          st = agent_cbor_skip_item(&r);
          if (st != AGENT_OK) return st;
        }

        if (!have_outbox_id) return AGENT_ERR_INVALID_ARGUMENT;

        if (have_msg && msg_item.ptr && msg_item.len) {
          row.msg_item = msg_item;
          st = agent_umbmp_envelope_read_cbor_v0_1(msg_item.ptr, msg_item.len, &row.msg);
          if (st != AGENT_OK) {
            // If the platform included an unparseable envelope, treat as invalid to avoid
            // nodes acting on ambiguous bytes.
            return AGENT_ERR_INVALID_ARGUMENT;
          }
          row.has_msg = 1;
        } else {
          // Skip rows without a decoded msg (msg_raw case).
          continue;
        }

        if (parsed < rows_cap) {
          out_rows[parsed] = row;
          parsed++;
        } else {
          out->truncated = 1;
        }
      }

      out->messages_parsed = parsed;
      continue;
    }

    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (!have_ok || !have_node_id || !have_cursor_base || !have_cursor_next || !have_messages) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (!out->ok) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_cbor_reader_offset(&r) != len) return AGENT_ERR_INVALID_ARGUMENT;

  return AGENT_OK;
}

