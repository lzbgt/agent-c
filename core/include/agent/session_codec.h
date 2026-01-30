#pragma once

#include <stddef.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// JSON-free, line-based session codec intended for embedded-friendly persistence.
//
// Format v1 (UTF-8 text):
//
//   AGENT_SESSION\t1\n
//   M\t<role>\t<escaped_content>\n
//
// Where:
// - <role> is one of: system | user | assistant | tool
// - <escaped_content> uses C-style escapes:
//     \\  \n  \r  \t  \xHH
//   (tabs/newlines are escaped so each message occupies one line)
//
// Notes:
// - v1 intentionally stores role+content only (no message parts yet).
// - Hosts may filter/transform legacy "tool transcript markers" on load, if desired.

agent_status_t agent_session_codec_encode_v1(const agent_session_t* session, agent_string_t* out_text);

agent_status_t agent_session_codec_decode_v1(const char* data, size_t len, agent_session_t** out_session);

#ifdef __cplusplus
}  // extern "C"
#endif

