#include "edge_interop_endpoints.h"

#include "daemon_auth.h"
#include "edge_confidentiality.h"
#include "edge_manifest_bundle.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <string>

namespace agentd {

void handle_edge_node_manifest_bundle_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid node_id");
    return;
  }

  Json::Value bundle;
  std::string sign_err;
  if (!build_edge_node_manifest_bundle(cfg, db_or_null, *nid, &bundle, &sign_err)) {
    if (sign_err == "node not found" || sign_err == "node has no manifest") {
      resp->status = 404;
      resp->body = json_error_body(sign_err);
      return;
    }
    if (sign_err == "failed to parse stored manifest" || sign_err == "failed to hash manifest") {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = sign_err;
      resp->body = edge_json_stringify_compact(o);
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (sign_err.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(sign_err.substr(std::string("attest_sign_config_invalid:").size()));
    } else if (sign_err.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(sign_err.substr(std::string("attest_sign_failed:").size()));
    } else {
      o["error"] = "manifest_bundle_sign_failed";
      if (!sign_err.empty()) o["details"] = sign_err;
    }
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["bundle"] = bundle;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_node_manifest_bundle_send_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  const std::string target_node_id =
    args.isMember("target_node_id") && args["target_node_id"].isString() ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string subject_node_id =
    args.isMember("subject_node_id") && args["subject_node_id"].isString() ? trim_copy(args["subject_node_id"].asString()) : "";
  const std::string confidential_kid =
    args.isMember("confidential_kid") && args["confidential_kid"].isString() ? trim_copy(args["confidential_kid"].asString()) : "";
  if (target_node_id.empty() || !edge_id_is_safe(target_node_id) || subject_node_id.empty() || !edge_id_is_safe(subject_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid target_node_id or subject_node_id");
    return;
  }
  if (!confidential_kid.empty() && (confidential_kid.size() > 64 || !edge_id_is_safe(confidential_kid))) {
    resp->status = 400;
    resp->body = json_error_body("missing/invalid confidential_kid");
    return;
  }

  AgentDb::EdgeNodeRow target_row;
  std::string terr;
  if (!db_or_null->get_edge_node(target_node_id, &target_row, &terr)) {
    resp->status = 404;
    resp->body = json_error_body("target node not found");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_node_manifest_bundle(cfg, db_or_null, subject_node_id, &bundle, &berr)) {
    if (berr == "node not found" || berr == "node has no manifest") {
      resp->status = 404;
      resp->body = json_error_body(berr == "node not found" ? "subject node not found" : "subject node has no manifest");
      return;
    }
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "manifest_bundle_send_failed" : berr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)edge_unix_ms_now();
  env["type"] = "PLATFORM_MANIFEST_BUNDLE";
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(target_node_id);
  Json::Value body(Json::objectValue);
  body["subject_node_id"] = subject_node_id;
  body["bundle"] = bundle;
  env["body"] = body;
  if (!confidential_kid.empty()) {
    std::string ecode;
    std::string eerr;
    if (!edge_confidentiality_wrap_envelope_body(
          &env, cfg.edge_confidentiality_keys, confidential_kid, &ecode, &eerr)) {
      resp->status = (ecode == "unknown_confidential_kid") ? 400 : 500;
      resp->body = json_error_body(eerr.empty() ? "failed to encrypt manifest bundle" : eerr);
      return;
    }
  }

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = target_node_id;
  orow.ts_utc_ms = env["ts_utc_ms"].asInt64();
  orow.envelope_json = edge_json_stringify_compact(env);
  int64_t outbox_id = 0;
  std::string oerr;
  if (!db_or_null->insert_edge_outbox_message(orow, &outbox_id, &oerr)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue manifest bundle" : oerr;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["target_node_id"] = target_node_id;
  o["subject_node_id"] = subject_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["bundle"] = bundle;
  resp->body = edge_json_stringify_compact(o);
}

}  // namespace agentd
