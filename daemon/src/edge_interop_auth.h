#pragma once

#include <cstdint>
#include <string>

#include <json/json.h>

#include "http_util.h"
#include "runtime_config.h"
#include "agentd/http_types.h"

namespace agentd {

bool fixed_time_eq32(const uint8_t a[32], const uint8_t b[32]);

bool edge_auth_is_revoked(
  const DaemonConfig& cfg,
  const std::string& node_id,
  const std::string& kid,
  std::string* out_reason
);

std::string umbmp_result_attest_input_v0_1(
  const std::string& task_id,
  const std::string& step_id,
  const std::string& idempotency_key,
  const std::string& result_sha256,
  int64_t ts_utc_ms
);

bool verify_edge_envelope_auth_best_effort(
  const DaemonConfig& cfg,
  const Json::Value& env,
  int64_t now_utc_ms,
  int64_t ts_utc_ms,
  const std::string& from_id,
  const Json::Value& body,
  HttpResponse* resp
);

}  // namespace agentd
