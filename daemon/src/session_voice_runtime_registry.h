#pragma once

#include "session_voice_runtime_internal.h"

#include <memory>
#include <mutex>
#include <string>

namespace agentd {

std::mutex& voice_peer_runtime_registry_mutex();

std::shared_ptr<VoicePeerRuntime> voice_peer_runtime_lookup_locked(const std::string& session_id);

void voice_peer_runtime_store_locked(const std::string& session_id, const std::shared_ptr<VoicePeerRuntime>& st);

void voice_peer_runtime_erase_locked(const std::string& session_id);

}  // namespace agentd
