#pragma once

#include "session_voice_runtime_internal.h"

#include <memory>

namespace agentd {

std::shared_ptr<VoicePeerRuntime> make_voice_peer_runtime_state(
  const VoicePeerRuntimeSeed& seed
);

}  // namespace agentd
