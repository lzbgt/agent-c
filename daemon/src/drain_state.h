#pragma once

#include <cstdint>
#include <string>

namespace agentd {

void drain_begin(int64_t until_unix_ms, const std::string& reason);
void drain_end();
bool drain_is_active();
int64_t drain_until_unix_ms();
std::string drain_reason();

}  // namespace agentd
