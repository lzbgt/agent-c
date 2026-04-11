#pragma once

#include <string>

namespace agentd {

bool edge_consensus_cluster_id_matches(const std::string& a, const std::string& b);

bool edge_consensus_sha256_token_is_valid(const std::string& raw);

bool edge_consensus_sha256_token_matches(const std::string& a, const std::string& b);

bool edge_consensus_optional_sha256_token_matches(const std::string& a, const std::string& b);

}  // namespace agentd
