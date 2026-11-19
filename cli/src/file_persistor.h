#pragma once

#include "agent/persist.h"

#ifdef __cplusplus
extern "C" {
#endif

// File-backed persistor implemented in the host adapter (C++).
// Uses the host's existing session store logic (currently: .sess primary + .json optional).
//
// `sessions_root_dir_or_null`:
// - if NULL/empty, defaults to "~/.agent/sessions" (best-effort HOME-based)
agent_status_t agent_file_persistor_create(const char* sessions_root_dir_or_null, agent_persistor_t* out);

#ifdef __cplusplus
}  // extern "C"

// Native CLI progress for a file-backed save; does not change the generic C callback ABI.
agent_status_t agent_file_persistor_save_with_progress(const agent_persistor_t& persistor, const char* session_id,
                                                       const agent_session_t* session, bool* portable_published);
#endif

