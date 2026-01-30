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
#endif

