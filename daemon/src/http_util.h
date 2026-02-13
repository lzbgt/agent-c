#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace agentd {

std::string url_decode(std::string_view s);
std::optional<std::string> query_get(const std::string& query, const std::string& key);
bool string_to_bool(const std::string& s);
std::string content_type_from_path(const std::filesystem::path& p);

std::string trim_slashes(std::string s);
std::string header_get_ci(const std::map<std::string, std::string>& headers, const std::string& key_lc);
std::string bearer_token_from_auth_header(const std::string& auth);
std::string cookie_get(const std::map<std::string, std::string>& headers, const std::string& name);

}  // namespace agentd
