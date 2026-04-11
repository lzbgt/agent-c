#pragma once

#include <set>
#include <string>
#include <vector>

namespace agentd {

bool edge_consensus_member_node_id_is_valid(const std::string& raw);

bool edge_consensus_member_node_id_matches(const std::string& a, const std::string& b);

bool edge_consensus_member_node_ids_contains(
  const std::vector<std::string>& haystack,
  const std::string& needle
);

void edge_consensus_append_member_node_id_if_unique(
  std::vector<std::string>* out,
  const std::string& raw
);

std::vector<std::string> edge_consensus_normalize_member_node_ids(
  const std::vector<std::string>& in
);

std::vector<std::string> edge_consensus_normalize_peer_node_ids(
  const std::vector<std::string>& in,
  const std::string& self_node_id
);

std::set<std::string> edge_consensus_normalize_member_node_id_set(
  const std::vector<std::string>& in,
  const std::string& self_node_id,
  bool force_self
);

}  // namespace agentd
