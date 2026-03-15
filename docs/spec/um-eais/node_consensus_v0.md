# Node Consensus v0

Status: implemented foundation (deterministic host-side state machine + simulator)

Purpose:
- define the first peer-to-peer node-native consensus protocol surface without relying on the platform coordinator
- carry node identity and trust-root epoch material through votes and recovery
- provide a deterministic simulation harness before wiring the protocol onto live UM-BMP transport

Implemented proof:
- `tests/test_edge_node_consensus.cpp`
- `edge_node_consensus_tests`

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

This makes the protocol suitable for deterministic replay and partition/conflict simulation even before live transport wiring.

## Current coverage

The shipped host tests prove:
- frame JSON round-trip
- duplicate vote suppression
- split-brain / partition behavior where only the majority side commits
- quorum recovery with a higher term leader replacing an older one
- rejection of stale trust-epoch candidates until they recover to the current trust view

## Still open

This document does **not** claim live decentralized consensus is fully deployed. Remaining work:
- carry `edge_node_consensus_frame_v1` over UM-BMP inbox/outbox transport
- persist leader/term/recovery state in durable edge node surfaces
- define operator/debug endpoints for decentralized consensus observability
