#pragma once

#include <string>

namespace agentd {

std::string lower_copy(std::string s);
bool url_contains_ci(const std::string& url, const std::string& needle);

std::string trim_copy(const std::string& s);

std::string truncate_for_event(const std::string& s, size_t max_bytes, bool* out_truncated = nullptr);

}  // namespace agentd
