#pragma once

#include "cors.h"
#include "config_store.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "agent_db.h"

namespace agentd {

struct EdgeVerifyChainResult {
  bool verified = false;
  int verify_error_code = 0;
  int verify_error_depth = -1;
  std::string verify_error;
  std::string leaf_subject;
  std::string leaf_issuer;
  std::string leaf_sha256_hex;
  std::vector<std::string> chain_subjects;
  std::vector<std::string> chain_sha256_hex;
  std::vector<std::string> matched_root_kids;
};

bool verify_edge_cert_chain_against_roots(
  const DaemonConfig& cfg,
  const std::string& cert_pem,
  const std::vector<std::string>& untrusted_cert_pem,
  EdgeVerifyChainResult* out_result,
  std::string* out_error
);

void handle_config_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// Updates daemon-side defaults at runtime (model/base_url/timeout/proxy + optional provider keys).
// Requires daemon auth when enabled; otherwise allowed for local loopback dev.
void handle_config_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_trust_roots_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_trust_roots_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_trust_roots_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_cert_roots_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_cert_roots_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_cert_roots_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_cert_roots_verify_chain_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_node_binding_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_provision_node_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_revocations_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_revocations_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_auth_revocations_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
