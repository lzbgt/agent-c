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
- portable `agent_core` cluster-size normalization and peer/member-derived cluster-size helpers, reused by the daemon
  replica and live membership adoption path so firmware-native ports share the same minimum-one cluster-size boundary
  before applying quorum math
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
- durable membership bundles now carry immediate previous membership epoch/member-set lineage plus a bounded newest-first
  `membership_lineage` history, so partitioned or restarting nodes can verify the policy transitions they missed instead
  of only seeing the current epoch
- portable `agent_core` membership-lineage validation is now reused by daemon runtime-config load, so firmware-native
  ports and agentd agree that previous policy lineage is either zero/unknown or strictly older than the current epoch
- stale builtin runtime recovery now gates recent recovered snapshots by current policy membership epoch or bounded lineage,
  so daemon restarts preserve compatible old-policy evidence but clear stale runtime records from unrelated membership
  histories
- portable `agent_core` retry-delay and lineage-recovery helpers are now reused by the daemon node loop and stale runtime
  recovery path, so firmware-native ports can make the same retry/backoff and old-policy recovery decisions without
  copying C++ host logic
- portable `agent_core` vote-request, vote-grant, and leader-commit acceptance predicates are now reused by the daemon
  replica, so firmware-native ports share the same term, membership, trust-epoch, and voted-for gating decisions as
  agentd while keeping frame mutation host-local; those predicates now also enforce role identity, requiring vote
  requests from the candidate, vote grants from a distinct voter, and leader commits from the claimed leader
- leader-commit acceptance now also requires a portable quorum-witness check: the carried vote witnesses must include
  the leader and enough member/trust-valid unique witnesses for quorum before a follower adopts the commit
- portable `agent_core` candidate-commit and leader-activity observation gates are now reused by the daemon loop, so
  firmware-native ports share the same "no existing leader plus quorum" commit boundary and the same timestamped
  leader-commit activity boundary
- portable `agent_core` decision-digest equality and self-plus-grant vote-count helpers are reused by the daemon
  replica, and `agent_core_tests` now proves a core-only quorum election path without constructing the host C++
  `EdgeConsensusReplica`
- portable `agent_core` frame-id formatting, incoming-term advancement, and duplicate-frame drop predicates are now
  reused by the daemon replica, so firmware-native ports can share the same deterministic frame identity and replay
  state gates while keeping mutable seen-frame storage host-local
- portable `agent_core` incoming-term stale detection is now reused by the daemon replica before counting vote grants,
  keeping higher/lower/same-term branching aligned with firmware-native ports
- portable `agent_core` frame-routing classification is now reused by the daemon loop: vote grants unicast to the
  candidate when valid and not self, while vote requests and leader commits broadcast to peers
- portable `agent_core` identity and consensus-frame shape validation is now reused by the daemon parser/replica, so
  firmware-native ports can share the same schema, kind, frame id, term, digest, candidate, leader, and identity checks
  without carrying parallel host-only validation logic
- portable `agent_core` membership-policy header validation and member-set nonempty checks are now reused by the daemon
  membership parser, leaving JSON type handling host-side while sharing schema/cluster/member-set shape rules with
  firmware-native ports
- portable `agent_core` cluster-id equality is now reused by the daemon replica and live membership-adoption path, so
  cross-cluster frame rejection and policy-bundle ignore decisions share the same id-safety-aware comparison as
  firmware-native ports
- semantically rejected consensus frames do not poison the duplicate-frame cache, so an invalid early delivery cannot
  suppress a later valid retransmission with the same frame id and term
- portable `agent_core` node-loop timing gates are now reused by the daemon loop for leader heartbeats, leader-lease
  expiry, post-expiry recampaign cooldown, and campaign start/retry scheduling, so firmware-native ports can reuse the
  same deterministic scheduler predicates without copying host-loop arithmetic
- portable `agent_core` post-lease-expiry campaign retry biasing is now reused by the daemon loop, so firmware-native
  ports preserve the same bounded retry schedule after a stale leader is expired
- portable `agent_core` membership-policy adoption gating now requires a strictly newer epoch and keeps the running node
  in the adopted member set, so firmware-native ports can share the same live-bundle fail-closed boundary
- managed runtimes now consume delivered `PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE` outbox messages in the reusable node
  loop, adopt strictly-newer self-including membership policy without restart, reset stale election/leader state at the
  epoch boundary, and then converge consensus under the adopted member set and timing policy
- portable `agent_core` consensus outbox message classification is now reused by the managed runtime core, so embedded
  firmware and agentd select between relayed `CONSENSUS_FRAME`, delivered `PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE`, and
  ignored unrelated UM-BMP messages through the same exact message-type boundary
- portable `agent_core` committed-decision presence detection is now reused by the daemon runtime core and node loop, so
  firmware-native ports share the same whitespace-insensitive state gate for commit exit, leader heartbeat, and campaign
  scheduling decisions
- portable `agent_core` campaign-decision source selection and campaign-start gating are now reused by the daemon loop, so
  firmware-native ports share the same configured-decision vs last-known-decision fallback and the same
  decision/commit/timing boundary before starting a new election
- portable `agent_core` term and frame-sequence advancement helpers are now reused by the daemon replica, giving
  firmware-native ports the same monotonic counter transition and saturation behavior for election terms and frame IDs
- portable `agent_core` leader-commit witness filtering now gates member/trust-valid/unique witness counting before the
  existing quorum check, so firmware-native ports share the same duplicate-witness rejection boundary as agentd
- portable `agent_core` node-id matching now backs the daemon replica's leader-self check, so ports share validated
  nonempty node-token equality instead of raw string comparison
- the daemon consensus loop/replica now uses that same portable node-id matcher for remaining candidate/leader role
  checks, route self-filtering, duplicate witness leader detection, and live-membership self-inclusion gates
- consensus runtime config/store member-list de-duplication and default peer derivation now also reuse portable node-id
  matching, keeping runtime persistence/recovery member-set shaping aligned with the node loop
- membership bundle normalization, membership rotation merge handling, and runtime-config membership lineage parsing also
  reuse portable node-id matching, keeping durable policy surfaces aligned before firmware-native policy adoption
- same-term vote-request regrant checks now also reuse portable node-id matching, so future firmware-native replicas do
  not fork a raw equality path when deciding whether a node can revote for the same candidate
- stale builtin runtime recovery is now member-set-aware across current, previous, and lineage epochs, so a daemon restart
  preserves old runtime evidence only when the runtime node was a member of the matching policy epoch
- runtime effective-config comparison now uses portable consensus cluster/node/digest matchers and normalized member sets,
  keeping restart/reconcile drift checks aligned with firmware-native identity semantics
- node-loop leader activity and last-known decision updates now require semantic frame acceptance, so firmware-native
  ports do not refresh lease/recovery state from rejected but parseable leader commits
- leader heartbeat commits now use fresh frame ids instead of replaying the original commit id, preserving duplicate-frame
  suppression while still letting accepted heartbeats refresh follower lease state

## Still open

This document does **not** claim production-complete embedded consensus is complete. Remaining work:
- replace the current builtin daemon-hosted runtime with embedded / firmware-native adoption
- extend the shipped durable/live-adopted membership bundle into richer long-lived recovery policy beyond stale-runtime
  preservation and lineage-gated stale-runtime recovery for multi-hour or partition-heavy deployments
