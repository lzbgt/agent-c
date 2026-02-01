#include "agent/runner.h"

#include "agent_alloc.h"

#include <string.h>

static agent_status_t agent_collect_message_views(
  const agent_session_t* session,
  agent_message_view_t** out_views,
  size_t* out_count
) {
  if (!session || !out_views || !out_count) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_views = NULL;
  *out_count = 0;

  const size_t n = agent_session_message_count(session);
  if (n == 0) {
    return AGENT_OK;
  }

  agent_message_view_t* views = (agent_message_view_t*)agent_malloc(n * sizeof(agent_message_view_t));
  if (!views) {
    return AGENT_ERR_OOM;
  }
  memset(views, 0, n * sizeof(agent_message_view_t));

  for (size_t i = 0; i < n; i++) {
    agent_status_t st = agent_session_get_message(session, i, &views[i]);
    if (st != AGENT_OK) {
      agent_free(views);
      return st;
    }
  }

  *out_views = views;
  *out_count = n;
  return AGENT_OK;
}

agent_status_t agent_run_once(
  agent_session_t* session,
  const agent_provider_t* provider,
  const agent_run_options_t* options,
  agent_run_report_t* out_report
) {
  if (!session || !provider || (!provider->generate && !provider->generate_ex) || !options || !options->model) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  agent_run_report_t report;
  memset(&report, 0, sizeof(report));

  // Compact before calling the provider (seamless compaction).
  agent_compact_report_t compact = {0};
  agent_status_t st = agent_session_compact_char_budget(
    session,
    options->max_chars == 0 ? 20000 : options->max_chars,
    options->keep_last_messages == 0 ? 16 : options->keep_last_messages,
    options->summary_or_null,
    &compact
  );
  if (st != AGENT_OK) {
    return st;
  }
  report.compact = compact;

  agent_message_view_t* views = NULL;
  size_t view_count = 0;
  st = agent_collect_message_views(session, &views, &view_count);
  if (st != AGENT_OK) {
    return st;
  }

  agent_generate_request_t req;
  req.model = options->model;
  req.messages = views;
  req.message_count = view_count;

  agent_generate_response_t resp;
  memset(&resp, 0, sizeof(resp));

  report.provider_called = 1;
  if (provider->generate_ex) {
    agent_generate_request_ex_t req_ex;
    req_ex.model = options->model;
    req_ex.session = session;
    req_ex.messages = views;
    req_ex.message_count = view_count;
    st = provider->generate_ex(provider->ctx, &req_ex, &resp);
  } else {
    st = provider->generate(provider->ctx, &req, &resp);
  }

  agent_free(views);

  if (st != AGENT_OK) {
    agent_string_free(&resp.assistant_text);
    return st;
  }
  if (!resp.assistant_text.data) {
    agent_string_free(&resp.assistant_text);
    return AGENT_ERR_INTERNAL;
  }

  st = agent_session_add_message(session, AGENT_ROLE_ASSISTANT, resp.assistant_text.data);
  agent_string_free(&resp.assistant_text);
  if (st != AGENT_OK) {
    return st;
  }

  // Report the appended message view (points to session-owned storage).
  const size_t n = agent_session_message_count(session);
  if (n > 0) {
    (void)agent_session_get_message(session, n - 1, &report.assistant_view);
  }

  if (out_report) {
    *out_report = report;
  }
  return AGENT_OK;
}
