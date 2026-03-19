#include "session_voice_runtime_registry.h"

#include <unordered_map>

namespace agentd {
namespace {

std::mutex g_voice_peer_mu;
std::unordered_map<std::string, std::shared_ptr<VoicePeerRuntime>> g_voice_peer_by_session;

}  // namespace

std::mutex& voice_peer_runtime_registry_mutex() { return g_voice_peer_mu; }

std::shared_ptr<VoicePeerRuntime> voice_peer_runtime_lookup_locked(const std::string& session_id) {
  const auto it = g_voice_peer_by_session.find(session_id);
  return it == g_voice_peer_by_session.end() ? nullptr : it->second;
}

void voice_peer_runtime_store_locked(const std::string& session_id, const std::shared_ptr<VoicePeerRuntime>& st) {
  if (!st) {
    g_voice_peer_by_session.erase(session_id);
    return;
  }
  g_voice_peer_by_session[session_id] = st;
}

void voice_peer_runtime_erase_locked(const std::string& session_id) { g_voice_peer_by_session.erase(session_id); }

}  // namespace agentd
