#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace agentd {

class AgentDb;

std::string build_health_body(std::chrono::steady_clock::time_point start_time);
std::string build_ready_body(std::chrono::steady_clock::time_point start_time, const AgentDb* db);
std::string build_metrics_body(std::chrono::steady_clock::time_point start_time, const AgentDb* db);

}  // namespace agentd
