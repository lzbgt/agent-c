# Edge Consensus Firmware Adoption Contract

This contract defines the boundary between firmware-owned consensus state,
portable `agent_core` rules, and platform-owned delivery state for
`edge_node_consensus_frame_v1`.

## Portable APIs

Firmware MUST consume the portable helpers in `core/include/agent/edge_interop.h`
instead of depending on the host C++ replica in `daemon/src/edge_node_consensus.*`.
The required helper groups are:

- Identity and frame validation:
  `agent_edge_consensus_identity_validate`,
  `agent_edge_consensus_frame_validate`,
  `agent_edge_consensus_frame_kind_is_valid`, and
  `agent_edge_consensus_frame_id_format`.
- Cluster, membership, and trust checks:
  `agent_edge_consensus_cluster_id_matches`,
  `agent_edge_consensus_node_id_matches`,
  `agent_edge_consensus_identity_membership_matches`,
  `agent_edge_consensus_member_node_id_is_valid`, and
  `agent_edge_consensus_trust_epochs_match`.
- Election and commit decisions:
  `agent_edge_consensus_vote_request_can_grant`,
  `agent_edge_consensus_vote_grant_can_count`,
  `agent_edge_consensus_vote_count_with_self`,
  `agent_edge_consensus_has_quorum`,
  `agent_edge_consensus_candidate_can_commit`,
  `agent_edge_consensus_leader_commit_can_accept`,
  `agent_edge_consensus_leader_commit_witness_can_count`, and
  `agent_edge_consensus_leader_commit_witnesses_can_accept`.
- Replay, routing, and term guards:
  `agent_edge_consensus_seen_frame_should_drop`,
  `agent_edge_consensus_incoming_term_advances`,
  `agent_edge_consensus_incoming_term_is_stale`, and
  `agent_edge_consensus_frame_route`.
- Membership and timing policy:
  `agent_edge_consensus_membership_policy_header_validate`,
  `agent_edge_consensus_membership_policy_can_adopt`,
  `agent_edge_consensus_membership_epoch_is_recoverable`,
  `agent_edge_consensus_membership_epoch_member_set_can_recover`,
  `agent_edge_consensus_policy_timing_normalize`, and the campaign,
  heartbeat, and lease helpers in the same header.

## Firmware-Owned State

Firmware owns the mutable replica state that changes as frames arrive:

- local identity: `cluster_id`, `node_id`, `manifest_sha256`,
  `membership_epoch`, `trust_roots_epoch`, `revocations_epoch`, and
  `cert_roots_epoch`
- current term, local frame sequence, voted-for node, known leader, campaign
  decision SHA, committed decision SHA, grant witnesses, committed witnesses,
  and a bounded seen-frame window keyed by `(frame_id, term)`
- the current member set and local cluster size derived with
  `agent_edge_consensus_cluster_size_from_member_count`
- local timers and retry counters after normalization with
  `agent_edge_consensus_policy_timing_normalize`

Firmware MUST reset term-scoped consensus state when adopting a newer
membership bundle or observing a newer term. It MAY keep transport cursors and
other delivery state, because those are not consensus decisions.

## Platform-Owned Delivery State

The platform owns durable delivery and operator policy:

- enqueueing, fanout, dedupe cursors, and retransmission of `CONSENSUS_FRAME`
  messages through `POST /api/v1/edge/message` and node outboxes
- signing, serving, rotating, and replaying
  `edge_consensus_membership_v1` bundles
- auth, confidential transport, node manifest binding, and revocation policy
- durable cluster defaults for campaign retry, heartbeat, leader lease, and
  membership recovery grace

The platform MUST NOT be the source of truth for a firmware node's current term,
vote, leader, or commit decision once the firmware loop is running. It may report
those fields as observations from node health/runtime snapshots.

## Frame Processing

Firmware frame handling MUST follow this order:

1. Validate schema, kind, frame id, term, decision SHA, sender identity, and
   candidate or leader node id with `agent_edge_consensus_frame_validate`.
2. Reject mismatched clusters and non-member or wrong-epoch senders using the
   portable cluster and membership helpers.
3. Drop exact `(frame_id, term)` replays through
   `agent_edge_consensus_seen_frame_should_drop`.
4. For `vote_request`, grant only when
   `agent_edge_consensus_vote_request_can_grant` passes, then advance to the
   request term if needed and emit a `vote_grant`.
5. For `vote_grant`, ignore stale terms, count only grants accepted by
   `agent_edge_consensus_vote_grant_can_count`, and emit `leader_commit` only
   when `agent_edge_consensus_has_quorum` and
   `agent_edge_consensus_candidate_can_commit` both pass.
6. For `leader_commit`, accept only if
   `agent_edge_consensus_leader_commit_can_accept` and the witness quorum
   helpers pass, then store the leader and committed decision.

`agent_edge_consensus_frame_route` defines relay intent only. Firmware still
must run validation and local state checks after a routed frame arrives.

## Membership And Recovery

Firmware membership adoption MUST use signed platform bundles as policy input,
then apply portable checks locally:

- validate the bundle header with
  `agent_edge_consensus_membership_policy_header_validate`
- require a non-empty member set and valid member node ids
- accept only monotonic membership epochs with
  `agent_edge_consensus_membership_policy_can_adopt`
- accept recovery only when the runtime epoch is current, previous, or in the
  lineage allowed by `agent_edge_consensus_membership_epoch_is_recoverable`
- require the local node to remain a member when recovering an equal epoch member
  set with `agent_edge_consensus_membership_epoch_member_set_can_recover`

After adopting a newer member set, firmware resets term-scoped election and
commit state and recomputes cluster size from the adopted member count.

## Verification

The firmware adoption proof is
`agent_core_edge_consensus_firmware_loop_tests`. It links only `agent_core`,
includes only `agent/edge_interop.h` for production consensus rules, and covers
duplicate, dropped, and reordered consensus frames plus duplicate/reordered
membership bundles across the portable-core boundary.
