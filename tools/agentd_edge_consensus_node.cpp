#include "edge_consensus_http_runtime.h"

#include <json/json.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace agentd;

namespace {

struct Options {
  std::string daemon_url;
  std::string auth_token;
  std::string node_id;
  std::string cluster_id;
  std::string manifest_sha256;
  std::string model = "edge_consensus_node";
  std::string fw_git_sha = "consensus-node";
  std::string decision_sha256;
  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  size_t cluster_size = 0;
  size_t outbox_limit = 128;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 1500;
  int64_t campaign_retry_max_ms = 1500;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t poll_interval_ms = 100;
  int64_t deadline_ms = 10000;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
  uint64_t membership_epoch = 0;
  bool verbose = false;
};

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static std::string json_compact(const Json::Value& root) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  wb["commentStyle"] = "None";
  return Json::writeString(wb, root);
}

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_edge_consensus_node") << "\n"
    << "  --daemon-url <url>\n"
    << "  --node-id <id>\n"
    << "  --cluster-id <id>\n"
    << "  --manifest-sha256 <sha256:...>\n"
    << "  [--peer-node-id <id>]...\n"
    << "  [--member-node-id <id>]...\n"
    << "  [--cluster-size <n>]\n"
    << "  [--decision-sha256 <sha256:...>]\n"
    << "  [--campaign-delay-ms <ms>]\n"
    << "  [--campaign-retry-ms <ms>]\n"
    << "  [--campaign-retry-max-ms <ms>]\n"
    << "  [--campaign-retry-backoff-factor <n>]\n"
    << "  [--poll-interval-ms <ms>]\n"
    << "  [--deadline-ms <ms>]\n"
    << "  [--trust-roots-epoch <n>]\n"
    << "  [--revocations-epoch <n>]\n"
    << "  [--cert-roots-epoch <n>]\n"
    << "  [--membership-epoch <n>]\n"
    << "  [--auth-token <token>]\n"
    << "  [--verbose]\n";
}

static bool parse_u64_arg(const std::string& text, uint64_t* out) {
  if (!out) return false;
  try {
    *out = (uint64_t)std::stoull(text);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_i64_arg(const std::string& text, int64_t* out) {
  if (!out) return false;
  try {
    *out = (int64_t)std::stoll(text);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_args(int argc, char** argv, Options* out) {
  if (!out) return false;
  Options opt;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--daemon-url" && i + 1 < argc) {
      opt.daemon_url = argv[++i];
    } else if (a == "--auth-token" && i + 1 < argc) {
      opt.auth_token = argv[++i];
    } else if (a == "--node-id" && i + 1 < argc) {
      opt.node_id = argv[++i];
    } else if (a == "--cluster-id" && i + 1 < argc) {
      opt.cluster_id = argv[++i];
    } else if (a == "--manifest-sha256" && i + 1 < argc) {
      opt.manifest_sha256 = argv[++i];
    } else if (a == "--model" && i + 1 < argc) {
      opt.model = argv[++i];
    } else if (a == "--fw-git-sha" && i + 1 < argc) {
      opt.fw_git_sha = argv[++i];
    } else if (a == "--peer-node-id" && i + 1 < argc) {
      opt.peer_node_ids.push_back(argv[++i]);
    } else if (a == "--member-node-id" && i + 1 < argc) {
      opt.member_node_ids.push_back(argv[++i]);
    } else if (a == "--decision-sha256" && i + 1 < argc) {
      opt.decision_sha256 = argv[++i];
    } else if (a == "--cluster-size" && i + 1 < argc) {
      uint64_t v = 0;
      if (!parse_u64_arg(argv[++i], &v) || v < 1) {
        std::cerr << "invalid --cluster-size\n";
        return false;
      }
      opt.cluster_size = (size_t)v;
    } else if (a == "--outbox-limit" && i + 1 < argc) {
      uint64_t v = 0;
      if (!parse_u64_arg(argv[++i], &v) || v < 1 || v > 2048) {
        std::cerr << "invalid --outbox-limit\n";
        return false;
      }
      opt.outbox_limit = (size_t)v;
    } else if (a == "--campaign-delay-ms" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.campaign_delay_ms) || opt.campaign_delay_ms < 0) {
        std::cerr << "invalid --campaign-delay-ms\n";
        return false;
      }
    } else if (a == "--campaign-retry-ms" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.campaign_retry_ms) || opt.campaign_retry_ms < 0) {
        std::cerr << "invalid --campaign-retry-ms\n";
        return false;
      }
    } else if (a == "--campaign-retry-max-ms" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.campaign_retry_max_ms) || opt.campaign_retry_max_ms < 0) {
        std::cerr << "invalid --campaign-retry-max-ms\n";
        return false;
      }
    } else if (a == "--campaign-retry-backoff-factor" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.campaign_retry_backoff_factor) || opt.campaign_retry_backoff_factor < 1) {
        std::cerr << "invalid --campaign-retry-backoff-factor\n";
        return false;
      }
    } else if (a == "--poll-interval-ms" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.poll_interval_ms) || opt.poll_interval_ms < 1) {
        std::cerr << "invalid --poll-interval-ms\n";
        return false;
      }
    } else if (a == "--deadline-ms" && i + 1 < argc) {
      if (!parse_i64_arg(argv[++i], &opt.deadline_ms) || opt.deadline_ms < 1) {
        std::cerr << "invalid --deadline-ms\n";
        return false;
      }
    } else if (a == "--trust-roots-epoch" && i + 1 < argc) {
      if (!parse_u64_arg(argv[++i], &opt.trust_roots_epoch)) {
        std::cerr << "invalid --trust-roots-epoch\n";
        return false;
      }
    } else if (a == "--revocations-epoch" && i + 1 < argc) {
      if (!parse_u64_arg(argv[++i], &opt.revocations_epoch)) {
        std::cerr << "invalid --revocations-epoch\n";
        return false;
      }
    } else if (a == "--cert-roots-epoch" && i + 1 < argc) {
      if (!parse_u64_arg(argv[++i], &opt.cert_roots_epoch)) {
        std::cerr << "invalid --cert-roots-epoch\n";
        return false;
      }
    } else if (a == "--membership-epoch" && i + 1 < argc) {
      if (!parse_u64_arg(argv[++i], &opt.membership_epoch)) {
        std::cerr << "invalid --membership-epoch\n";
        return false;
      }
    } else if (a == "--verbose") {
      opt.verbose = true;
    } else if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      return false;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return false;
    }
  }

  if (trim_copy(opt.daemon_url).empty() || trim_copy(opt.node_id).empty() || trim_copy(opt.cluster_id).empty() ||
      trim_copy(opt.manifest_sha256).empty()) {
    print_usage(argv[0]);
    return false;
  }
  if (opt.cluster_size == 0) opt.cluster_size = opt.peer_node_ids.size() + 1;
  *out = opt;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) return 2;

  EdgeConsensusHttpRuntimeConfig cfg;
  cfg.daemon_url = opt.daemon_url;
  cfg.auth_token = opt.auth_token;
  cfg.node_id = opt.node_id;
  cfg.cluster_id = opt.cluster_id;
  cfg.manifest_sha256 = opt.manifest_sha256;
  cfg.model = opt.model;
  cfg.fw_git_sha = opt.fw_git_sha;
  cfg.decision_sha256 = opt.decision_sha256;
  cfg.peer_node_ids = opt.peer_node_ids;
  cfg.member_node_ids = opt.member_node_ids;
  cfg.cluster_size = opt.cluster_size;
  cfg.outbox_limit = opt.outbox_limit;
  cfg.campaign_delay_ms = opt.campaign_delay_ms;
  cfg.campaign_retry_ms = opt.campaign_retry_ms;
  cfg.campaign_retry_max_ms = opt.campaign_retry_max_ms;
  cfg.campaign_retry_backoff_factor = opt.campaign_retry_backoff_factor;
  cfg.poll_interval_ms = opt.poll_interval_ms;
  cfg.deadline_ms = opt.deadline_ms;
  cfg.trust_roots_epoch = opt.trust_roots_epoch;
  cfg.revocations_epoch = opt.revocations_epoch;
  cfg.cert_roots_epoch = opt.cert_roots_epoch;
  cfg.membership_epoch = opt.membership_epoch;

  EdgeConsensusHttpRuntimeHooks hooks;
  hooks.log_line = [&](const std::string& line) {
    if (!opt.verbose) return;
    std::cerr << "[" << opt.node_id << "] " << line << "\n";
  };

  Json::Value result(Json::objectValue);
  std::string err;
  if (!run_edge_consensus_http_runtime(cfg, hooks, &result, &err)) {
    std::cerr << err << "\n";
    return 1;
  }

  std::cout << json_compact(result) << "\n";
  return result.isObject() && result.isMember("ok") && result["ok"].asBool() ? 0 : 1;
}
