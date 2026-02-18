#pragma once

#include "daemon_config.h"
#include "http_client.h"

#include <string>

namespace agentd {

bool blob_object_store_is_configured(const DaemonConfig& cfg, std::string* out_error);
bool blob_object_store_is_enabled(const DaemonConfig& cfg, std::string* out_error);

std::string blob_object_store_key_for_hex(const DaemonConfig& cfg, const std::string& hex);
std::string blob_object_store_key_for_blob_id(const DaemonConfig& cfg, const std::string& blob_id);

bool blob_object_store_presign_url(
  const DaemonConfig& cfg,
  const std::string& method,
  const std::string& key,
  int64_t now_utc_ms,
  int64_t expires_sec,
  std::string* out_url,
  std::string* out_error
);

bool blob_object_store_put(
  const DaemonConfig& cfg,
  const std::string& key,
  const std::string& body,
  const std::string& mime,
  int64_t now_utc_ms,
  std::string* out_etag,
  std::string* out_error
);

bool blob_object_store_delete(
  const DaemonConfig& cfg,
  const std::string& key,
  int64_t now_utc_ms,
  std::string* out_error
);

bool blob_object_store_get(
  const DaemonConfig& cfg,
  const std::string& key,
  const std::string& range_header,
  size_t max_bytes,
  int64_t now_utc_ms,
  HttpClientResult* out_result,
  std::string* out_error
);

}  // namespace agentd
