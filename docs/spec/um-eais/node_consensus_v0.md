# Node Consensus v0

Status: implemented rolling foundation (deterministic state machine + live UM-BMP relay/observability + autonomous host-side node loop)

Purpose:
- define the first peer-to-peer node-native consensus protocol surface without relying on the platform coordinator
- carry node identity and trust-root epoch material through votes and recovery
- provide a deterministic simulation harness plus live UM-BMP relay/observability foundation

Implemented proof:
- `tests/test_edge_node_consensus.cpp`
- `edge_node_consensus_tests`
- `tests/agentd_edge_consensus_transport_smoke.sh`
- `tools/agentd_edge_consensus_node.cpp`
- `tests/agentd_edge_consensus_autonomous_smoke.sh`

Source surface:
- `daemon/src/edge_node_consensus.h`
- `daemon/src/edge_node_consensus.cpp`

## Protocol frame

Schema:
- `edge_node_consensus_frame_v1`

Kinds:
- `vote_request`
- `vote_grant`
- `leader_commit`

Common fields:
- `schema`
- `frame_id`
- `kind`
- `term`
- `decision_sha256`
- `from`

`from` carries:
- `cluster_id`
- `node_id`
- `manifest_sha256`
- `trust_epochs`

`trust_epochs` carries:
- `trust_roots_epoch`
- `revocations_epoch`
- `cert_roots_epoch`

Additional fields by kind:
- `vote_request`
  - `candidate_node_id`
- `vote_grant`
  - `candidate_node_id`
  - `granted`
- `leader_commit`
  - `leader_node_id`
  - `vote_witnesses[]`

## Deterministic rules

Replica rules implemented in the shipped foundation:
- each replica grants at most one candidate per term
- higher terms reset prior local vote/campaign state
- duplicate frames are ignored by `frame_id` + `term`
- trust epochs must match exactly for votes or commits to be accepted
- quorum is strict majority: `floor(cluster_size / 2) + 1`
- a leader commit carries the quorum witness set, including node identity and trust epochs for each counted vote

This makes the protocol suitable for deterministic replay, partition/conflict simulation, and live
platform-relayed UM-BMP transport wiring.

## Current coverage

The shipped host tests prove:
- frame JSON round-trip
- duplicate vote suppression
- split-brain / partition behavior where only the majority side commits
- quorum recovery with a higher term leader replacing an older one
- rejection of stale trust-epoch candidates until they recover to the current trust view

The shipped live transport smoke proves:
- `CONSENSUS_FRAME` ingress through `POST /api/v1/edge/message`
- relay to recipient node outboxes through `GET /api/v1/edge/outbox`
- sender-side consensus summaries through `GET /api/v1/edge/node` and `GET /api/v1/edge/nodes`

The shipped autonomous host-loop proof adds:
- a real node-side poll/process/post loop on top of `EdgeConsensusReplica`
- autonomous `vote_request -> vote_grant -> leader_commit` progression over the live platform relay path
- per-node committed leader/decision proof from the node-loop stdout plus platform-side consensus summaries

## Still open

This document does **not** claim production-complete embedded consensus is complete. Remaining work:
- embed the shipped autonomous loop into long-lived node firmware/runtime rather than only the host bring-up tool
- add stronger operator/debug control surfaces beyond the current node-read summaries
- define membership, liveness, and retry timers for multi-hour or partition-heavy deployments
