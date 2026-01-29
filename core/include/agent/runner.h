#pragma once

#include "agent/agent.h"
#include "agent/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_run_options {
  const char* model;
  size_t max_chars;
  size_t keep_last_messages;
  const char* summary_or_null; // optional host-generated summary inserted during compaction
} agent_run_options_t;

typedef struct agent_run_report {
  agent_compact_report_t compact;
  uint8_t provider_called;
  agent_message_view_t assistant_view; // valid when provider_called=1 and call succeeded
} agent_run_report_t;

agent_status_t agent_run_once(
  agent_session_t* session,
  const agent_provider_t* provider,
  const agent_run_options_t* options,
  agent_run_report_t* out_report
);

#ifdef __cplusplus
}  // extern "C"
#endif

