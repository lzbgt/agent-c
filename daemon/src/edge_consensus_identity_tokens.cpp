#include "edge_consensus_identity_tokens.h"

#include "string_util.h"

#include "agent/edge_interop.h"

namespace agentd {

bool edge_consensus_cluster_id_matches(const std::string& a, const std::string& b) {
  const std::string left = trim_copy(a);
  const std::string right = trim_copy(b);
  return agent_edge_consensus_cluster_id_matches(left.data(), left.size(), right.data(), right.size()) == 1;
}

bool edge_consensus_sha256_token_is_valid(const std::string& raw) {
  const std::string token = trim_copy(raw);
  return agent_umbmp_sha256_token_is_safe(token.data(), token.size()) == 1;
}

bool edge_consensus_sha256_token_matches(const std::string& a, const std::string& b) {
  const std::string left = trim_copy(a);
  const std::string right = trim_copy(b);
  return agent_edge_consensus_decision_sha256_matches(left.data(), left.size(), right.data(), right.size()) == 1;
}

bool edge_consensus_optional_sha256_token_matches(const std::string& a, const std::string& b) {
  const std::string left = trim_copy(a);
  const std::string right = trim_copy(b);
  if (left.empty() && right.empty()) return true;
  return edge_consensus_sha256_token_matches(left, right);
}

}  // namespace agentd
