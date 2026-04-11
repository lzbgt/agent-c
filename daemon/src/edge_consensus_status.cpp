#include "edge_consensus_status.h"

#include "agent_db.h"
#include "edge_node_consensus.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <unordered_set>

namespace agentd {
namespace {

Json::Value parse_health_json_best_effort(const std::string& health_json) {
  if (health_json.empty()) return Json::Value(Json::objectValue);
  Json::Value out(Json::nullValue);
  std::string err;
  if (!json_parse_any(health_json, &out, &err) || !out.isObject()) {
    return Json::Value(Json::objectValue);
  }
  return out;
}

}  // namespace

bool collect_edge_consensus_target_node_ids(
  const Json::Value& body,
  const std::string& env_to,
  std::vector<std::string>* out_node_ids,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_node_ids) return false;
  out_node_ids->clear();
  std::unordered_set<std::string> seen;
  auto push_node_id = [&](const std::string& node_id) -> bool {
    const std::string nid = trim_copy(node_id);
    if (nid.empty()) return true;
    if (!edge_id_is_safe(nid)) {
      if (out_error) *out_error = "invalid consensus target_node_id";
      return false;
    }
    if (seen.insert(nid).second) out_node_ids->push_back(nid);
    return true;
  };

  if (body.isMember("target_node_id") && !body["target_node_id"].isNull()) {
    if (!body["target_node_id"].isString()) {
      if (out_error) *out_error = "target_node_id must be string";
      return false;
    }
    if (!push_node_id(body["target_node_id"].asString())) return false;
  }
  if (body.isMember("target_node_ids") && !body["target_node_ids"].isNull()) {
    if (!body["target_node_ids"].isArray()) {
      if (out_error) *out_error = "target_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < body["target_node_ids"].size(); i++) {
      if (!body["target_node_ids"][i].isString()) {
        if (out_error) *out_error = "target_node_ids entries must be strings";
        return false;
      }
      if (!push_node_id(body["target_node_ids"][i].asString())) return false;
    }
  }
  if (out_node_ids->empty() && env_to.rfind("node:", 0) == 0 && env_to.size() > 5) {
    if (!push_node_id(env_to.substr(5))) return false;
  }
  return true;
}

bool upsert_edge_node_consensus_health(
  AgentDb* db,
  const std::string& node_id,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& target_node_ids,
  const std::string& original_msg_id,
  int64_t now_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  AgentDb::EdgeNodeRow row;
  std::string err;
  if (!db->get_edge_node(node_id, &row, &err)) row.node_id = node_id;
  Json::Value health = parse_health_json_best_effort(row.health_json);
  Json::Value consensus(Json::objectValue);
  if (health.isMember("consensus") && health["consensus"].isObject()) {
    consensus = health["consensus"];
  }
  consensus["schema"] = "edge_node_consensus_status_v1";
  consensus["updated_utc_ms"] = (Json::Int64)now_utc_ms;
  if (!original_msg_id.empty()) consensus["last_msg_id"] = original_msg_id;
  consensus["last_frame_id"] = frame.frame_id;
  consensus["last_frame_kind"] = frame.kind;
  consensus["current_term"] = Json::UInt64(frame.term);
  if (!frame.decision_sha256.empty()) consensus["decision_sha256"] = frame.decision_sha256;
  if (!frame.candidate_node_id.empty()) consensus["candidate_node_id"] = frame.candidate_node_id;
  if (!frame.leader_node_id.empty()) consensus["leader_node_id"] = frame.leader_node_id;
  if (frame.kind == "vote_grant") consensus["granted"] = frame.granted;
  consensus["from"] = edge_consensus_identity_to_json(frame.from);
  if (!target_node_ids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    consensus["target_node_ids"] = arr;
    consensus["forwarded_count"] = (Json::UInt64)target_node_ids.size();
  } else {
    consensus["forwarded_count"] = (Json::UInt64)0;
    consensus.removeMember("target_node_ids");
  }
  if (!frame.vote_witnesses.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& witness : frame.vote_witnesses) {
      Json::Value w(Json::objectValue);
      w["node_id"] = witness.node_id;
      if (!witness.manifest_sha256.empty()) w["manifest_sha256"] = witness.manifest_sha256;
      w["trust_epochs"] = edge_consensus_epochs_to_json(witness.trust_epochs);
      arr.append(w);
    }
    consensus["vote_witnesses"] = arr;
    consensus["vote_witness_count"] = (Json::UInt64)frame.vote_witnesses.size();
  } else {
    consensus["vote_witness_count"] = (Json::UInt64)0;
    consensus.removeMember("vote_witnesses");
  }
  health["consensus"] = consensus;
  row.health_json = edge_json_stringify_compact(health);
  return db->upsert_edge_node(row, out_error);
}

}  // namespace agentd
