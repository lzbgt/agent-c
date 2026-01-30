#pragma once

#include <string>

namespace agentd {

// Returns a client-specific system prompt snippet, or an empty string when no profile exists.
// This is used to give the agent default presentation/DoD guidance for the active collaboration surface
// (WebUI, Slack, mobile, etc.).
std::string client_profile_system_prompt(const std::string& client_kind);

}  // namespace agentd

