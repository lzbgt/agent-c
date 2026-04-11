#include "edge_platform_bundle.h"

#include "agent_db.h"
#include "edge_confidentiality.h"
#include "edge_util.h"

namespace agentd {

bool enqueue_edge_platform_bundle(
  AgentDb* db_or_null,
  const std::string& target_node_id,
  const std::string& msg_type,
  const char* body_field,
  const Json::Value& bundle,
  const std::map<std::string, std::string>* confidentiality_keys_or_null,
  const std::string& confidential_kid,
  int64_t* out_outbox_id,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_outbox_id) *out_outbox_id = 0;
  if (!db_or_null || !db_or_null->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id)) {
    if (out_error) *out_error = "missing/invalid target_node_id";
    return false;
  }
  if (msg_type.empty() || !body_field || std::string(body_field).empty()) {
    if (out_error) *out_error = "internal error";
    return false;
  }

  AgentDb::EdgeNodeRow target_row;
  std::string terr;
  if (!db_or_null->get_edge_node(target_node_id, &target_row, &terr)) {
    if (out_error) *out_error = "target node not found";
    return false;
  }

  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  env["type"] = msg_type;
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(target_node_id);
  Json::Value body(Json::objectValue);
  body[body_field] = bundle;
  env["body"] = body;
  if (!confidential_kid.empty()) {
    std::string ecode;
    std::string eerr;
    const std::map<std::string, std::string> empty_keys;
    if (!edge_confidentiality_wrap_envelope_body(
          &env,
          confidentiality_keys_or_null ? *confidentiality_keys_or_null : empty_keys,
          confidential_kid,
          &ecode,
          &eerr)) {
      if (out_error) *out_error = eerr.empty() ? ecode : eerr;
      return false;
    }
  }

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = target_node_id;
  orow.ts_utc_ms = env["ts_utc_ms"].asInt64();
  orow.envelope_json = edge_json_stringify_compact(env);
  std::string oerr;
  if (!db_or_null->insert_edge_outbox_message(orow, out_outbox_id, &oerr)) {
    if (out_error) *out_error = oerr.empty() ? "failed to enqueue bundle" : oerr;
    return false;
  }
  return true;
}

}  // namespace agentd
