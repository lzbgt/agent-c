#pragma once

#include "daemon_config.h"
#include "memory_salience.h"
#include "openai_client.h"

#include <json/json.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace agentd {

struct MemoryRecapOptions {
  MemorySaliencePolicy salience;
  std::string model; // optional override; falls back to summary_model when empty
  std::string kind;  // optional label: daily|weekly|manual
  size_t summary_max_chars = 1200;
  bool dry_run = false;
  bool write_file = true;
  std::function<bool()> cancelled;
};

struct MemoryRecapReport {
  int64_t generated_utc_ms = 0;
  std::string ts_utc;
  std::string model;
  std::string kind;
  bool dry_run = false;
  bool write_file = true;
  MemorySaliencePolicy policy;
  MemorySalienceReport salience;
  std::string summary_text;
  Json::Value summary_json;
  bool summary_json_ok = false;
  std::string recap_path_rel;
  int64_t recap_bytes = 0;
  std::string prompt;
  bool prompt_truncated = false, complete = false, publication_uncertain = false;
  int dropped_inputs = 0;
  long provider_http_status = 0;
  std::string model_response, returned_model, provider_response_id, response_body_sha256,
      provider_error;
  bool response_capture_complete = true;
  std::string provider_outcome = "not_called", recap_revision, recap_source_id;
  Json::Value source_state, derivation, usage;
};

bool memory_generate_recap(const DaemonConfig &cfg, const OpenAIClientConfig &ocfg,
                           const MemoryRecapOptions &opt, MemoryRecapReport *out_report,
                           std::string *out_err);

bool memory_list_recaps(const DaemonConfig &cfg, int limit, bool include_summary,
                        const std::string &kind, host_tools_internal::MemoryViewMode view,
                        Json::Value *out_list, Json::Value *metadata, std::string *out_err);

Json::Value memory_recap_report_to_json(const MemoryRecapReport &rep, bool include_prompt);

class MemoryRecapEngine {
public:
  struct Options {
    int poll_ms = 1000;
  };

  MemoryRecapEngine(std::function<DaemonConfig()> cfg_snapshot,
                    std::function<OpenAIClientConfig(const DaemonConfig &)> ocfg_from_cfg,
                    Options opt);
  ~MemoryRecapEngine();

  MemoryRecapEngine(const MemoryRecapEngine &) = delete;
  MemoryRecapEngine &operator=(const MemoryRecapEngine &) = delete;

  bool start(std::string *out_error);
  void stop();

private:
  void worker_main();

  std::function<DaemonConfig()> cfg_snapshot_;
  std::function<OpenAIClientConfig(const DaemonConfig &)> ocfg_from_cfg_;
  Options opt_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

} // namespace agentd
