#include "edge_consensus_member_ids.h"

#include "string_util.h"

#include "agent/edge_interop.h"

#include <algorithm>

namespace agentd {

bool edge_consensus_member_node_id_is_valid(const std::string& raw) {
  const std::string node_id = trim_copy(raw);
  return agent_edge_consensus_member_node_id_is_valid(node_id.data(), node_id.size()) == 1;
}

bool edge_consensus_member_node_id_matches(const std::string& a, const std::string& b) {
  const std::string left = trim_copy(a);
  const std::string right = trim_copy(b);
  return agent_edge_consensus_node_id_matches(left.data(), left.size(), right.data(), right.size()) == 1;
}

bool edge_consensus_member_node_ids_contains(
  const std::vector<std::string>& haystack,
  const std::string& needle
) {
  if (!edge_consensus_member_node_id_is_valid(needle)) return false;
  return std::any_of(haystack.begin(), haystack.end(), [&](const std::string& item) {
    return edge_consensus_member_node_id_matches(item, needle);
  });
}

void edge_consensus_append_member_node_id_if_unique(
  std::vector<std::string>* out,
  const std::string& raw
) {
  if (!out) return;
  const std::string node_id = trim_copy(raw);
  if (!edge_consensus_member_node_id_is_valid(node_id)) return;
  if (!edge_consensus_member_node_ids_contains(*out, node_id)) out->push_back(node_id);
}

std::vector<std::string> edge_consensus_normalize_member_node_ids(
  const std::vector<std::string>& in
) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    edge_consensus_append_member_node_id_if_unique(&out, raw);
  }
  return out;
}

std::vector<std::string> edge_consensus_normalize_peer_node_ids(
  const std::vector<std::string>& in,
  const std::string& self_node_id
) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    const std::string node_id = trim_copy(raw);
    if (!edge_consensus_member_node_id_is_valid(node_id)) continue;
    if (edge_consensus_member_node_id_matches(node_id, self_node_id)) continue;
    if (!edge_consensus_member_node_ids_contains(out, node_id)) out.push_back(node_id);
  }
  return out;
}

std::set<std::string> edge_consensus_normalize_member_node_id_set(
  const std::vector<std::string>& in,
  const std::string& self_node_id,
  bool force_self
) {
  std::set<std::string> out;
  if (force_self && edge_consensus_member_node_id_is_valid(self_node_id)) {
    out.insert(trim_copy(self_node_id));
  }
  for (const auto& member : edge_consensus_normalize_member_node_ids(in)) {
    out.insert(member);
  }
  return out;
}

}  // namespace agentd
