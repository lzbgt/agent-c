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
- `membership_epoch`
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
- membership epoch must match exactly and the sender/leader/candidate must be in the local member set
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
- managed runtime ownership through `POST /api/v1/edge/node/consensus_runtime` and
  `GET /api/v1/edge/node/consensus_runtime?node_id=...`
- mirrored `consensus_runtime` visibility on `GET /api/v1/edge/node` and `GET /api/v1/edge/nodes`

The shipped autonomous host-loop proof adds:
- a real node-side poll/process/post loop on top of `EdgeConsensusReplica`
- autonomous `vote_request -> vote_grant -> leader_commit` progression over the live platform relay path
- per-node committed leader/decision proof from the node-loop stdout plus platform-side consensus summaries
- agentd-managed long-lived host runtime ownership of the same loop, including explicit start/status/stop control
- a reusable transport-agnostic node loop core (`EdgeConsensusNodeLoop`) so the election/commit state machine is no
  longer stranded inside the host CLI helper
- retry-capable campaign timers (`campaign_delay_ms`, `campaign_retry_ms`, `campaign_retry_max_ms`,
  `campaign_retry_backoff_factor`) so a candidate can start before peers are online and still converge later without
  manual restart while capping retry growth
- leader freshness policy (`leader_heartbeat_ms`, `leader_lease_ms`) so committed leaders periodically reaffirm the
  current decision and followers can deterministically expire stale leaders before re-campaigning
- durable post-expiry recovery policy (`lease_expiry_recampaign_delay_ms`) so followers can wait out a bounded
  cooldown after leader-lease expiry instead of always re-campaigning immediately during transient partitions
- stale managed-runtime recovery policy (`stale_runtime_recovery_grace_ms`) so a daemon restart can preserve a recent
  stale builtin runtime as a terminal recovered snapshot instead of immediately deleting all runtime evidence
- explicit membership surfaces (`membership_epoch` + `member_node_ids`) on the shared core, host helper, and managed runtime
- signed/durable cluster policy bundles via `edge_consensus_membership_v1`, including outbox delivery as
  `PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE`
- managed runtime defaulting from the stored cluster policy when explicit member-set / retry fields are omitted
- builtin managed-runtime execution inside agentd by default, with the standalone helper retained only as an explicit
  `runtime_kind=external` fallback/debug backend
- shared HTTP poll/process/post loop code (`run_edge_consensus_http_runtime`) reused by both the builtin runtime and the
  standalone helper so transport behavior is no longer stranded in the helper binary
- portable `agent_core` consensus constants and quorum math (`CONSENSUS_FRAME`,
  `PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE`, `edge_node_consensus_frame_v1`,
  `edge_consensus_membership_v1`) so embedded firmware and agentd share the same wire names and majority threshold
- portable `agent_core` consensus frame-kind and membership-epoch acceptance helpers, reused by the daemon replica so
  firmware-native ports and agentd use the same `vote_request` / `vote_grant` / `leader_commit` validation and
  "same epoch plus listed member" rule
- daemon relay/runtime/membership-bundle paths consume those portable message and schema constants rather than
  re-declaring the wire strings locally
- portable `agent_core` durable policy timing normalization, reused by the daemon node loop, runtime start, config load, and
  membership rotation so embedded firmware and agentd clamp campaign, lease, recovery, and stale-runtime grace policy
  consistently
- portable `agent_core` member-node-id validation and strictly-monotonic membership-epoch advancement checks, reused by
  daemon runtime parsing, membership bundle normalization/rotation/send, and the reusable node loop so firmware-native
  ports and agentd accept member identities and cluster-policy epochs consistently
- portable `agent_core` SHA-256 token and trust-epoch-match checks reused by daemon runtime parsing/persistence and the
  reusable node loop, keeping digest and trust recovery acceptance aligned with future firmware-native ports

## Still open

This document does **not** claim production-complete embedded consensus is complete. Remaining work:
- replace the current builtin daemon-hosted runtime with embedded / firmware-native adoption
- extend the shipped durable membership bundle into richer long-lived recovery policy beyond stale-runtime preservation
  for multi-hour or partition-heavy deployments
