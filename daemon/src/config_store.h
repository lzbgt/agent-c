#pragma once

#include "daemon_config.h"

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace agentd {

class DaemonConfigStore {
 public:
  explicit DaemonConfigStore(DaemonConfig cfg) : cfg_(std::move(cfg)) {}

  DaemonConfig snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return cfg_;
  }

  void replace(DaemonConfig cfg) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    cfg_ = std::move(cfg);
  }

 private:
  mutable std::shared_mutex mu_;
  DaemonConfig cfg_;
};

}  // namespace agentd
