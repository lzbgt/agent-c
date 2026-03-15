#include "edge_node_consensus.h"
#include "http_client.h"

#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
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
  size_t cluster_size = 0;
  size_t outbox_limit = 128;
  int64_t campaign_delay_ms = 0;
  int64_t poll_interval_ms = 100;
  int64_t deadline_ms = 10000;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
  bool verbose = false;
};

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}

static int64_t now_utc_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static std::string json_compact(const Json::Value& root) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  wb["commentStyle"] = "None";
  return Json::writeString(wb, root);
}

static bool parse_json_text(const std::string& text, Json::Value* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out) *out = Json::Value();
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  std::string errs;
  const auto raw = trim_copy(text);
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  Json::Value parsed;
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &parsed, &errs)) {
    if (out_error) *out_error = errs;
    return false;
  }
  if (out) *out = parsed;
  return true;
}

static std::map<std::string, std::string> header_map_with_json(const Options& opt) {
  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/json";
  if (!opt.auth_token.empty()) headers["Authorization"] = "Bearer " + opt.auth_token;
  return headers;
}

static std::string make_msg_id(const std::string& node_id, uint64_t seq) {
  return node_id + ":consensus-node:" + std::to_string(seq);
}

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_edge_consensus_node") << "\n"
    << "  --daemon-url <url>\n"
    << "  --node-id <id>\n"
    << "  --cluster-id <id>\n"
    << "  --manifest-sha256 <sha256:...>\n"
    << "  [--peer-node-id <id>]...\n"
    << "  [--cluster-size <n>]\n"
    << "  [--decision-sha256 <sha256:...>]\n"
    << "  [--campaign-delay-ms <ms>]\n"
    << "  [--poll-interval-ms <ms>]\n"
    << "  [--deadline-ms <ms>]\n"
    << "  [--trust-roots-epoch <n>]\n"
    << "  [--revocations-epoch <n>]\n"
    << "  [--cert-roots-epoch <n>]\n"
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

static bool http_get_json(const std::string& url, const Options& opt, Json::Value* out, std::string* out_error) {
  const HttpClientResult r =
    http_request(url, "GET", header_map_with_json(opt), "", 5000, 1024 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_error) {
      *out_error = !r.error.empty() ? r.error : "http status " + std::to_string((int)r.http_status);
    }
    return false;
  }
  return parse_json_text(r.response_body, out, out_error);
}

static bool http_post_json(
  const std::string& url,
  const Json::Value& body,
  const Options& opt,
  Json::Value* out,
  std::string* out_error
) {
  const HttpClientResult r =
    http_request(url, "POST", header_map_with_json(opt), json_compact(body), 5000, 1024 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_error) {
      *out_error = !r.error.empty() ? r.error : "http status " + std::to_string((int)r.http_status);
      if (!trim_copy(r.response_body).empty()) *out_error += " body=" + r.response_body;
    }
    return false;
  }
  if (out) return parse_json_text(r.response_body, out, out_error);
  return true;
}

static bool post_hello(const Options& opt, uint64_t* io_seq, std::string* out_error) {
  if (!io_seq) return false;
  Json::Value env(Json::objectValue);
  env["msg_id"] = make_msg_id(opt.node_id, ++(*io_seq));
  env["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  env["type"] = "NODE_HELLO";
  env["from"] = "node:" + opt.node_id;
  env["to"] = "platform";
  Json::Value body(Json::objectValue);
  body["node_id"] = opt.node_id;
  body["model"] = opt.model;
  body["fw_git_sha"] = opt.fw_git_sha;
  body["caps_sha256"] = opt.manifest_sha256;
  env["body"] = body;
  Json::Value resp;
  return http_post_json(join_base_path(opt.daemon_url, "/api/v1/edge/message"), env, opt, &resp, out_error);
}

static std::vector<std::string> dedupe_targets(const std::vector<std::string>& in, const std::string& self_node_id) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& raw : in) {
    const std::string nid = trim_copy(raw);
    if (nid.empty() || nid == self_node_id) continue;
    if (!seen.insert(nid).second) continue;
    out.push_back(nid);
  }
  return out;
}

static bool send_consensus_frame(
  const Options& opt,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  uint64_t* io_seq,
  std::string* out_error
) {
  if (!io_seq) return false;
  const std::vector<std::string> target_node_ids = dedupe_targets(raw_target_node_ids, opt.node_id);
  if (target_node_ids.empty()) return true;

  Json::Value env(Json::objectValue);
  env["msg_id"] = make_msg_id(opt.node_id, ++(*io_seq));
  env["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  env["type"] = "CONSENSUS_FRAME";
  env["from"] = "node:" + opt.node_id;
  env["to"] = "platform";

  Json::Value body(Json::objectValue);
  body["frame"] = edge_consensus_frame_to_json(frame);
  if (target_node_ids.size() == 1) {
    body["target_node_id"] = target_node_ids.front();
  } else {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    body["target_node_ids"] = arr;
  }
  env["body"] = body;

  Json::Value resp;
  return http_post_json(join_base_path(opt.daemon_url, "/api/v1/edge/message"), env, opt, &resp, out_error);
}

static bool poll_outbox(
  const Options& opt,
  int64_t cursor,
  Json::Value* out,
  std::string* out_error
) {
  const std::string url = join_base_path(
    opt.daemon_url,
    "/api/v1/edge/outbox?node_id=" + opt.node_id + "&cursor=" + std::to_string(cursor) +
      "&limit=" + std::to_string((int)opt.outbox_limit)
  );
  return http_get_json(url, opt, out, out_error);
}

static std::vector<std::string> targets_for_generated_frame(const Options& opt, const EdgeConsensusFrame& frame) {
  if (frame.kind == "vote_grant") return {frame.candidate_node_id};
  if (frame.kind == "leader_commit" || frame.kind == "vote_request") return opt.peer_node_ids;
  return {};
}

static void log_verbose(const Options& opt, const std::string& line) {
  if (!opt.verbose) return;
  std::cerr << "[" << opt.node_id << "] " << line << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) return 2;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    std::cerr << "curl_global_init failed\n";
    return 1;
  }

  uint64_t msg_seq = 0;
  std::string err;
  if (!post_hello(opt, &msg_seq, &err)) {
    std::cerr << "failed to post NODE_HELLO: " << err << "\n";
    curl_global_cleanup();
    return 1;
  }

  EdgeConsensusIdentity self;
  self.cluster_id = opt.cluster_id;
  self.node_id = opt.node_id;
  self.manifest_sha256 = opt.manifest_sha256;
  self.trust_epochs.trust_roots_epoch = opt.trust_roots_epoch;
  self.trust_epochs.revocations_epoch = opt.revocations_epoch;
  self.trust_epochs.cert_roots_epoch = opt.cert_roots_epoch;

  EdgeConsensusReplica replica(self, opt.cluster_size);
  const int64_t started_ms = now_utc_ms();
  const int64_t deadline_at = started_ms + opt.deadline_ms;
  int64_t cursor = 0;
  bool election_started = false;

  while (now_utc_ms() < deadline_at) {
    const int64_t elapsed_ms = now_utc_ms() - started_ms;
    if (!election_started && !trim_copy(opt.decision_sha256).empty() && elapsed_ms >= opt.campaign_delay_ms) {
      const EdgeConsensusFrame request = replica.start_election(opt.decision_sha256);
      std::vector<std::string> targets = dedupe_targets(opt.peer_node_ids, opt.node_id);
      if (!send_consensus_frame(opt, request, targets, &msg_seq, &err)) {
        std::cerr << "failed to send vote_request: " << err << "\n";
        curl_global_cleanup();
        return 1;
      }
      log_verbose(opt, "sent vote_request term=" + std::to_string((unsigned long long)request.term));
      election_started = true;
    }

    Json::Value outbox;
    if (!poll_outbox(opt, cursor, &outbox, &err)) {
      std::cerr << "failed to poll outbox: " << err << "\n";
      curl_global_cleanup();
      return 1;
    }
    if (outbox.isMember("cursor_next") && outbox["cursor_next"].isInt64()) {
      cursor = std::max(cursor, outbox["cursor_next"].asInt64());
    }

    bool processed_message = false;
    if (outbox.isMember("messages") && outbox["messages"].isArray()) {
      for (Json::ArrayIndex i = 0; i < outbox["messages"].size(); i++) {
        const Json::Value row = outbox["messages"][i];
        if (!row.isObject() || !row.isMember("msg") || !row["msg"].isObject()) continue;
        const Json::Value env = row["msg"];
        const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
        if (type != "CONSENSUS_FRAME") continue;
        const Json::Value body = env.isMember("body") && env["body"].isObject() ? env["body"] : Json::Value(Json::objectValue);
        if (!body.isMember("frame") || !body["frame"].isObject()) continue;
        EdgeConsensusFrame frame;
        std::string ferr;
        if (!edge_consensus_frame_from_json(body["frame"], &frame, &ferr)) {
          std::cerr << "invalid relayed consensus frame: " << ferr << "\n";
          curl_global_cleanup();
          return 1;
        }
        std::vector<EdgeConsensusFrame> generated;
        std::string herr;
        if (!replica.handle_frame(frame, &generated, &herr)) {
          std::cerr << "failed to handle relayed frame: " << herr << "\n";
          curl_global_cleanup();
          return 1;
        }
        log_verbose(opt, "handled " + frame.kind + " from " + frame.from.node_id + " term=" +
                             std::to_string((unsigned long long)frame.term));
        for (const auto& out_frame : generated) {
          const std::vector<std::string> targets = targets_for_generated_frame(opt, out_frame);
          if (!send_consensus_frame(opt, out_frame, targets, &msg_seq, &err)) {
            std::cerr << "failed to send generated frame: " << err << "\n";
            curl_global_cleanup();
            return 1;
          }
          log_verbose(opt, "sent " + out_frame.kind + " term=" + std::to_string((unsigned long long)out_frame.term));
        }
        processed_message = true;
      }
    }

    if (!trim_copy(replica.committed_decision_sha256()).empty()) {
      Json::Value result(Json::objectValue);
      result["ok"] = true;
      result["node_id"] = opt.node_id;
      result["leader_node_id"] = replica.leader_node_id();
      result["committed_decision_sha256"] = replica.committed_decision_sha256();
      result["current_term"] = Json::UInt64(replica.current_term());
      result["status"] = replica.status_to_json();
      std::cout << json_compact(result) << "\n";
      curl_global_cleanup();
      return 0;
    }

    if (!processed_message) std::this_thread::sleep_for(std::chrono::milliseconds(opt.poll_interval_ms));
  }

  Json::Value result(Json::objectValue);
  result["ok"] = false;
  result["node_id"] = opt.node_id;
  result["error"] = "deadline exceeded before commit";
  result["current_term"] = Json::UInt64(replica.current_term());
  result["status"] = replica.status_to_json();
  std::cout << json_compact(result) << "\n";
  curl_global_cleanup();
  return 1;
}
