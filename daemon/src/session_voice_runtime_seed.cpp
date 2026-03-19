#include "session_voice_runtime_seed.h"

#include <chrono>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

std::shared_ptr<VoicePeerRuntime> make_voice_peer_runtime_state(
  const VoicePeerRuntimeSeed& seed
) {
  auto st = std::make_shared<VoicePeerRuntime>();
  st->runtime_kind = seed.runtime_kind;
  st->session_id = seed.session_id;
  st->broker_session_id = seed.broker_session_id;
  st->broker_url = seed.broker_url;
  st->broker_agent_id = seed.broker_agent_id;
  st->broker_deployment_id = seed.broker_deployment_id;
  st->sender_tag = seed.sender_tag;
  st->tool_path = seed.tool_path;
  st->node_bin = seed.node_bin;
  st->ready_file_path = seed.ready_file_path;
  st->stdout_log_path = seed.stdout_log_path;
  st->stderr_log_path = seed.stderr_log_path;
  st->started_unix_ms = now_unix_ms();
  st->deadline_ms = seed.deadline_ms;
  st->poll_interval_ms = seed.poll_interval_ms;
  st->tone_hz = seed.tone_hz;
  st->managed_broker_session = seed.managed_broker_session;
  st->ready = seed.ready;
  st->running = seed.running;
  st->pid = seed.pid;
  return st;
}

}  // namespace agentd
