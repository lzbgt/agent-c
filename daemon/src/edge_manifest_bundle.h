#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

class AgentDb;
struct DaemonConfig;

bool build_edge_manifest_identity_cert_verify(
  const DaemonConfig& cfg,
  const Json::Value& manifest,
  Json::Value* out_verify,
  bool* out_has_cert,
  std::string* out_error
);

bool build_edge_node_manifest_bundle(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const std::string& node_id,
  Json::Value* out_bundle,
  std::string* out_error
);

}  // namespace agentd
