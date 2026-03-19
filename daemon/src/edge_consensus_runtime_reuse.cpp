#include "edge_consensus_runtime_reuse.h"

#include "string_util.h"

namespace agentd {

bool edge_consensus_runtime_evaluate_reuse(
  const DaemonConfig& cfg,
  const Json::Value& body,
  const std::string& runtime_kind,
  const EdgeConsensusRuntime& current,
  EdgeConsensusRuntimeReuseResult* out,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out) {
    if (out_err) *out_err = "reuse result output missing";
    return false;
  }
  *out = EdgeConsensusRuntimeReuseResult{};
  out->runtime = edge_consensus_runtime_response_json(cfg, current);
  if (!current.running) return true;

  std::string build_err;
  if (!edge_consensus_runtime_build_config(cfg, body, &out->desired_config, &out->desired_state, &build_err)) {
    out->disposition = EdgeConsensusRuntimeReuseDisposition::invalid_request;
    out->error = build_err.empty() ? "failed to validate requested consensus runtime config" : build_err;
    return true;
  }

  out->desired_state.runtime_kind = runtime_kind;
  out->desired_state.tool_path = runtime_kind == "external" ? trim_copy(cfg.edge_consensus_node_tool_path) : "@builtin";

  if (!edge_consensus_runtime_same_effective_config(current, out->desired_state)) {
    out->disposition = EdgeConsensusRuntimeReuseDisposition::conflict;
    out->error = "consensus runtime already running with different config";
    return true;
  }

  out->disposition = EdgeConsensusRuntimeReuseDisposition::reusable;
  return true;
}

}  // namespace agentd
