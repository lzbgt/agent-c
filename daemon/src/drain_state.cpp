#include "drain_state.h"

#include <atomic>
#include <mutex>

namespace agentd {
namespace {

struct DrainState {
  std::atomic<bool> active{false};
  std::atomic<int64_t> until_unix_ms{0};
  std::mutex mu;
  std::string reason;
};

DrainState g_drain;

}  // namespace

void drain_begin(int64_t until_unix_ms, const std::string& reason) {
  g_drain.active.store(true, std::memory_order_release);
  g_drain.until_unix_ms.store(until_unix_ms, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(g_drain.mu);
    g_drain.reason = reason;
  }
}

void drain_end() {
  g_drain.active.store(false, std::memory_order_release);
  g_drain.until_unix_ms.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(g_drain.mu);
    g_drain.reason.clear();
  }
}

bool drain_is_active() {
  return g_drain.active.load(std::memory_order_acquire);
}

int64_t drain_until_unix_ms() {
  return g_drain.until_unix_ms.load(std::memory_order_acquire);
}

std::string drain_reason() {
  std::lock_guard<std::mutex> lk(g_drain.mu);
  return g_drain.reason;
}

}  // namespace agentd
