#include "agent/edge_interop.h"

#include <assert.h>
#include <string.h>

static void test_id_is_safe_basic(void) {
  assert(agent_umbmp_id_is_safe("wf:abc-XYZ_09.", strlen("wf:abc-XYZ_09.")) == 1);
  assert(agent_umbmp_id_is_safe("edge_msg:1234", strlen("edge_msg:1234")) == 1);
  assert(agent_umbmp_id_is_safe("", 0) == 0);
  assert(agent_umbmp_id_is_safe("has space", strlen("has space")) == 0);
  assert(agent_umbmp_id_is_safe("has/slash", strlen("has/slash")) == 0);
  assert(agent_umbmp_id_is_safe("has\"quote", strlen("has\"quote")) == 0);
}

static void test_trace_id_is_safe_allows_at(void) {
  assert(agent_umbmp_trace_id_is_safe("trace:demo@x", strlen("trace:demo@x")) == 1);
  assert(agent_umbmp_trace_id_is_safe("trace:bad space", strlen("trace:bad space")) == 0);
  // '@' is NOT allowed for general ids.
  assert(agent_umbmp_id_is_safe("trace:demo@x", strlen("trace:demo@x")) == 0);
}

static void test_sha256_token_is_safe_basic(void) {
  assert(agent_umbmp_sha256_token_is_safe("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                          strlen("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) == 1);
  assert(agent_umbmp_sha256_token_is_safe("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                          strlen("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) == 1);
  assert(agent_umbmp_sha256_token_is_safe("sha256:zzz", strlen("sha256:zzz")) == 0);
  assert(agent_umbmp_sha256_token_is_safe("", 0) == 0);
}

static void test_sanitize_trims_and_defaults(void) {
  char out[64];
  size_t n = 0;

  assert(agent_umbmp_sanitize_id_token("___hello$$$", strlen("___hello$$$"), out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "hello") == 0);
  assert(n == strlen("hello"));

  assert(agent_umbmp_sanitize_id_token("$$$", strlen("$$$"), out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "msg") == 0);
  assert(n == strlen("msg"));

  assert(agent_umbmp_sanitize_id_token(NULL, 0, out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "msg") == 0);
  assert(n == strlen("msg"));
}

static void test_sanitize_respects_max_len(void) {
  char out[16];
  size_t n = 0;
  const char* in = "abcdefghijklmnopqrstuvwxyz";
  assert(agent_umbmp_sanitize_id_token(in, strlen(in), out, sizeof(out), 8, &n) == AGENT_OK);
  assert(n == 8);
  assert(strcmp(out, "abcdefgh") == 0);
}

static void test_result_attest_signing_input_v0_1(void) {
  char out[256];
  size_t out_len = 0;

  const char* task_id = "t1";
  const char* step_id = "s1";
  const char* idem = "k1";
  const char* sha = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const int64_t ts = 1700000000000LL;

  assert(agent_um_eais_result_attest_signing_input_v0_1(
           task_id, strlen(task_id),
           step_id, strlen(step_id),
           idem, strlen(idem),
           sha, strlen(sha),
           ts,
           out, sizeof(out),
           &out_len) == AGENT_OK);

  const char* expected =
    "UM_EAIS_RESULT_ATTEST_v0_1\n"
    "t1\n"
    "s1\n"
    "k1\n"
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "1700000000000\n";
  assert(out_len == strlen(expected));
  assert(strcmp(out, expected) == 0);

  // Reject unsafe IDs (newline would be ambiguous).
  assert(agent_um_eais_result_attest_signing_input_v0_1(
           "bad\nid", strlen("bad\nid"),
           step_id, strlen(step_id),
           idem, strlen(idem),
           sha, strlen(sha),
           ts,
           out, sizeof(out),
           &out_len) == AGENT_ERR_INVALID_ARGUMENT);
}

static void test_consensus_constants_and_quorum(void) {
  assert(strcmp(AGENT_UM_BMP_TYPE_CONSENSUS_FRAME, "CONSENSUS_FRAME") == 0);
  assert(strcmp(
           AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE,
           "PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, "edge_node_consensus_frame_v1") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1, "edge_consensus_membership_v1") == 0);
  assert(strcmp(
           AGENT_EDGE_CONSENSUS_MEMBERSHIP_ATTEST_SCHEMA_V1,
           "edge_consensus_membership_attest_v1") == 0);
  assert(AGENT_EDGE_CONSENSUS_MEMBERSHIP_LINEAGE_MAX == 8);
  assert(strcmp(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, "vote_request") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT, "vote_grant") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT, "leader_commit") == 0);

  assert(agent_edge_consensus_quorum_for_cluster_size(0) == 1);
  assert(agent_edge_consensus_quorum_for_cluster_size(1) == 1);
  assert(agent_edge_consensus_quorum_for_cluster_size(2) == 2);
  assert(agent_edge_consensus_quorum_for_cluster_size(3) == 2);
  assert(agent_edge_consensus_quorum_for_cluster_size(4) == 3);
  assert(agent_edge_consensus_quorum_for_cluster_size(5) == 3);

  assert(agent_edge_consensus_has_quorum(3, 1) == 0);
  assert(agent_edge_consensus_has_quorum(3, 2) == 1);
  assert(agent_edge_consensus_has_quorum(4, 2) == 0);
  assert(agent_edge_consensus_has_quorum(4, 3) == 1);
  assert(agent_edge_consensus_frame_kind_is_valid("vote_request", strlen("vote_request")) == 1);
  assert(agent_edge_consensus_frame_kind_is_valid("vote_grant", strlen("vote_grant")) == 1);
  assert(agent_edge_consensus_frame_kind_is_valid("leader_commit", strlen("leader_commit")) == 1);
  assert(agent_edge_consensus_frame_kind_is_valid("leader_commitx", strlen("leader_commitx")) == 0);
  assert(agent_edge_consensus_frame_kind_is_valid(NULL, 0) == 0);
  {
    char frame_id[AGENT_UM_BMP_MAX_ID_LEN + 1];
    size_t frame_id_len = 0;
    assert(agent_edge_consensus_frame_id_format(
             "node-a", strlen("node-a"),
             AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST,
             strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
             42,
             NULL,
             0,
             frame_id,
             sizeof(frame_id),
             &frame_id_len) == AGENT_OK);
    assert(strcmp(frame_id, "node-a:vote_request:42") == 0);
    assert(frame_id_len == strlen(frame_id));
    assert(agent_edge_consensus_frame_id_format(
             "node-a", strlen("node-a"),
             AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
             strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT),
             7,
             "node-b",
             strlen("node-b"),
             frame_id,
             sizeof(frame_id),
             &frame_id_len) == AGENT_OK);
    assert(strcmp(frame_id, "node-a:vote_grant:7:node-b") == 0);
    assert(agent_edge_consensus_frame_id_format(
             "bad/node", strlen("bad/node"),
             AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST,
             strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
             1,
             NULL,
             0,
             frame_id,
             sizeof(frame_id),
             &frame_id_len) == AGENT_ERR_INVALID_ARGUMENT);
    assert(agent_edge_consensus_frame_id_format(
             "node-a", strlen("node-a"),
             "bad_kind",
             strlen("bad_kind"),
             1,
             NULL,
             0,
             frame_id,
             sizeof(frame_id),
             &frame_id_len) == AGENT_ERR_INVALID_ARGUMENT);
    assert(agent_edge_consensus_frame_id_format(
             "node-a", strlen("node-a"),
             AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT,
             strlen(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT),
             1,
             "bad/node",
             strlen("bad/node"),
             frame_id,
             sizeof(frame_id),
             &frame_id_len) == AGENT_ERR_INVALID_ARGUMENT);
    assert(agent_edge_consensus_frame_id_format(
             "node-a", strlen("node-a"),
             AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT,
             strlen(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT),
             1,
             "node-b",
             strlen("node-b"),
             frame_id,
             8,
             &frame_id_len) == AGENT_ERR_BOUNDS);
  }
  assert(agent_edge_consensus_identity_membership_matches(7, 7, 1) == 1);
  assert(agent_edge_consensus_identity_membership_matches(7, 8, 1) == 0);
  assert(agent_edge_consensus_identity_membership_matches(7, 7, 0) == 0);
  assert(agent_edge_consensus_trust_epochs_match(1, 2, 3, 1, 2, 3) == 1);
  assert(agent_edge_consensus_trust_epochs_match(1, 2, 3, 4, 2, 3) == 0);
  assert(agent_edge_consensus_trust_epochs_match(1, 2, 3, 1, 4, 3) == 0);
  assert(agent_edge_consensus_trust_epochs_match(1, 2, 3, 1, 2, 4) == 0);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 4, 1, 1, 1, "node-b", strlen("node-b"), "node-a", strlen("node-a")) == 1);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 3, 1, 1, 1, "", 0, "node-a", strlen("node-a")) == 1);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 3, 1, 1, 1, "node-a", strlen("node-a"), "node-a", strlen("node-a")) == 1);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 3, 1, 1, 1, "node-b", strlen("node-b"), "node-a", strlen("node-a")) == 0);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 2, 1, 1, 1, "", 0, "node-a", strlen("node-a")) == 0);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 4, 0, 1, 1, "", 0, "node-a", strlen("node-a")) == 0);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 4, 1, 0, 1, "", 0, "node-a", strlen("node-a")) == 0);
  assert(agent_edge_consensus_vote_request_can_grant(
           3, 4, 1, 1, 0, "", 0, "node-a", strlen("node-a")) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 1, 0, 1, 1, 1) == 1);
  assert(agent_edge_consensus_vote_grant_can_count(3, 2, 1, 1, 0, 1, 1, 1) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 0, 0, 1, 1, 1) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 1, 1, 1, 1, 1) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 1, 0, 0, 1, 1) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 1, 0, 1, 0, 1) == 0);
  assert(agent_edge_consensus_vote_grant_can_count(3, 3, 1, 1, 0, 1, 1, 0) == 0);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 3, 1, 1, 1) == 1);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 4, 1, 1, 1) == 1);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 2, 1, 1, 1) == 0);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 3, 0, 1, 1) == 0);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 3, 1, 0, 1) == 0);
  assert(agent_edge_consensus_leader_commit_can_accept(3, 3, 1, 1, 0) == 0);
  assert(agent_edge_consensus_leader_commit_witnesses_can_accept(3, 2, 1) == 1);
  assert(agent_edge_consensus_leader_commit_witnesses_can_accept(3, 1, 1) == 0);
  assert(agent_edge_consensus_leader_commit_witnesses_can_accept(3, 2, 0) == 0);
  assert(agent_edge_consensus_leader_commit_witnesses_can_accept(1, 1, 1) == 1);
  assert(agent_edge_consensus_incoming_term_advances(3, 4) == 1);
  assert(agent_edge_consensus_incoming_term_advances(3, 3) == 0);
  assert(agent_edge_consensus_incoming_term_advances(3, 2) == 0);
  assert(agent_edge_consensus_seen_frame_should_drop(1, 3, 3) == 1);
  assert(agent_edge_consensus_seen_frame_should_drop(1, 3, 4) == 0);
  assert(agent_edge_consensus_seen_frame_should_drop(0, 3, 3) == 0);
  assert(agent_edge_consensus_frame_route(
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST,
           strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           0,
           0) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS);
  assert(agent_edge_consensus_frame_route(
           AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT,
           strlen(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT),
           0,
           0) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS);
  assert(agent_edge_consensus_frame_route(
           AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
           strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT),
           1,
           0) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_CANDIDATE);
  assert(agent_edge_consensus_frame_route(
           AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
           strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT),
           1,
           1) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP);
  assert(agent_edge_consensus_frame_route(
           AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
           strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT),
           0,
           0) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP);
  assert(agent_edge_consensus_frame_route("unknown", strlen("unknown"), 1, 0) ==
         AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP);
  assert(agent_edge_consensus_identity_validate(
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           strlen("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) ==
         AGENT_EDGE_CONSENSUS_IDENTITY_OK);
  assert(agent_edge_consensus_identity_validate(
           "bad/cluster", strlen("bad/cluster"),
           "node-a", strlen("node-a"),
           NULL,
           0) == AGENT_EDGE_CONSENSUS_IDENTITY_CLUSTER_ID_INVALID);
  assert(agent_edge_consensus_identity_validate(
           "cluster-a", strlen("cluster-a"),
           "bad/node", strlen("bad/node"),
           NULL,
           0) == AGENT_EDGE_CONSENSUS_IDENTITY_NODE_ID_INVALID);
  assert(agent_edge_consensus_identity_validate(
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           "sha256:bad", strlen("sha256:bad")) == AGENT_EDGE_CONSENSUS_IDENTITY_MANIFEST_SHA256_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           strlen("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_OK);
  assert(agent_edge_consensus_frame_validate(
           "bad_schema", strlen("bad_schema"),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           "bad_kind", strlen("bad_kind"),
           "node-a:bad:1", strlen("node-a:bad:1"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_KIND_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "bad/frame", strlen("bad/frame"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_ID_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:0", strlen("node-a:vote_request:0"),
           0,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_TERM_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           "sha256:bad", strlen("sha256:bad"),
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_DECISION_SHA256_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           NULL, 0,
           "bad/cluster", strlen("bad/cluster"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_FROM_CLUSTER_ID_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "bad/node", strlen("bad/node"),
           NULL, 0,
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_FROM_NODE_ID_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           "sha256:bad", strlen("sha256:bad"),
           "node-a", strlen("node-a"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_FROM_MANIFEST_SHA256_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST, strlen(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST),
           "node-a:vote_request:1", strlen("node-a:vote_request:1"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           "bad/node", strlen("bad/node"),
           NULL, 0) == AGENT_EDGE_CONSENSUS_FRAME_CANDIDATE_NODE_ID_INVALID);
  assert(agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, strlen(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT, strlen(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT),
           "node-a:leader_commit:1:node-a", strlen("node-a:leader_commit:1:node-a"),
           1,
           NULL, 0,
           "cluster-a", strlen("cluster-a"),
           "node-a", strlen("node-a"),
           NULL, 0,
           NULL, 0,
           "bad/node", strlen("bad/node")) == AGENT_EDGE_CONSENSUS_FRAME_LEADER_NODE_ID_INVALID);
  assert(agent_edge_consensus_member_node_id_is_valid("node-a", strlen("node-a")) == 1);
  assert(agent_edge_consensus_member_node_id_is_valid("cluster:node_1", strlen("cluster:node_1")) == 1);
  assert(agent_edge_consensus_member_node_id_is_valid("bad/node", strlen("bad/node")) == 0);
  assert(agent_edge_consensus_member_node_id_is_valid("", 0) == 0);
  assert(agent_edge_consensus_membership_epoch_can_advance(7, 8) == 1);
  assert(agent_edge_consensus_membership_epoch_can_advance(7, 7) == 0);
  assert(agent_edge_consensus_membership_epoch_can_advance(8, 7) == 0);
  assert(agent_edge_consensus_membership_policy_can_adopt(7, 8, 1) == 1);
  assert(agent_edge_consensus_membership_policy_can_adopt(7, 7, 1) == 0);
  assert(agent_edge_consensus_membership_policy_can_adopt(7, 8, 0) == 0);
  assert(agent_edge_consensus_membership_policy_header_validate(
           AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1,
           strlen(AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1),
           "cluster-a",
           strlen("cluster-a")) == AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK);
  assert(agent_edge_consensus_membership_policy_header_validate(
           "bad_schema",
           strlen("bad_schema"),
           "cluster-a",
           strlen("cluster-a")) == AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_SCHEMA_INVALID);
  assert(agent_edge_consensus_membership_policy_header_validate(
           AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1,
           strlen(AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1),
           "bad/cluster",
           strlen("bad/cluster")) == AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_CLUSTER_ID_INVALID);
  assert(agent_edge_consensus_membership_member_set_is_nonempty(1) == 1);
  assert(agent_edge_consensus_membership_member_set_is_nonempty(0) == 0);
  assert(agent_edge_consensus_membership_lineage_is_valid(0, 0) == 1);
  assert(agent_edge_consensus_membership_lineage_is_valid(0, 19) == 1);
  assert(agent_edge_consensus_membership_lineage_is_valid(18, 19) == 1);
  assert(agent_edge_consensus_membership_lineage_is_valid(19, 19) == 0);
  assert(agent_edge_consensus_membership_lineage_is_valid(20, 19) == 0);
  const uint64_t lineage[] = {18, 17};
  assert(agent_edge_consensus_membership_epoch_is_recoverable(0, 19, 18, lineage, 2) == 1);
  assert(agent_edge_consensus_membership_epoch_is_recoverable(19, 19, 18, lineage, 2) == 1);
  assert(agent_edge_consensus_membership_epoch_is_recoverable(18, 19, 18, lineage, 2) == 1);
  assert(agent_edge_consensus_membership_epoch_is_recoverable(17, 19, 18, lineage, 2) == 1);
  assert(agent_edge_consensus_membership_epoch_is_recoverable(16, 19, 18, lineage, 2) == 0);
  assert(agent_edge_consensus_membership_epoch_is_recoverable(16, 0, 0, NULL, 0) == 1);
}

static void test_consensus_policy_timing_normalize(void) {
  agent_edge_consensus_policy_timing_t timing;
  timing.campaign_delay_ms = -5;
  timing.campaign_retry_ms = 130000;
  timing.campaign_retry_max_ms = 1;
  timing.campaign_retry_backoff_factor = 99;
  timing.leader_heartbeat_ms = 130000;
  timing.leader_lease_ms = 100;
  timing.lease_expiry_recampaign_delay_ms = 500000;
  timing.stale_runtime_recovery_grace_ms = 90000000;
  assert(agent_edge_consensus_policy_timing_normalize(&timing) == AGENT_OK);
  assert(timing.campaign_delay_ms == 0);
  assert(timing.campaign_retry_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(timing.campaign_retry_max_ms == timing.campaign_retry_ms);
  assert(timing.campaign_retry_backoff_factor == AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  assert(timing.leader_heartbeat_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(timing.leader_lease_ms == timing.leader_heartbeat_ms);
  assert(timing.lease_expiry_recampaign_delay_ms == AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  assert(timing.stale_runtime_recovery_grace_ms == AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS);

  timing.campaign_delay_ms = 42;
  timing.campaign_retry_ms = 1000;
  timing.campaign_retry_max_ms = 600000;
  timing.campaign_retry_backoff_factor = 0;
  timing.leader_heartbeat_ms = 0;
  timing.leader_lease_ms = -1;
  timing.lease_expiry_recampaign_delay_ms = -7;
  timing.stale_runtime_recovery_grace_ms = -8;
  assert(agent_edge_consensus_policy_timing_normalize(&timing) == AGENT_OK);
  assert(timing.campaign_delay_ms == 42);
  assert(timing.campaign_retry_ms == 1000);
  assert(timing.campaign_retry_max_ms == AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  assert(timing.campaign_retry_backoff_factor == AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MIN);
  assert(timing.leader_heartbeat_ms == 0);
  assert(timing.leader_lease_ms == 0);
  assert(timing.lease_expiry_recampaign_delay_ms == 0);
  assert(timing.stale_runtime_recovery_grace_ms == 0);
  assert(agent_edge_consensus_policy_timing_normalize(NULL) == AGENT_ERR_INVALID_ARGUMENT);

  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 8000, 2, 0) == 0);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 8000, 2, 1) == 1000);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 8000, 2, 2) == 2000);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 8000, 2, 4) == 8000);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 8000, 2, 5) == 8000);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 0, 2, 3) == 1000);
  assert(agent_edge_consensus_campaign_retry_delay_ms(1000, 999999999, 99, 4) == 512000);
}

static void test_consensus_loop_timing_gates(void) {
  assert(agent_edge_consensus_leader_heartbeat_due(1000, 5000, 0, 1, 1) == 1);
  assert(agent_edge_consensus_leader_heartbeat_due(1000, 5000, 4200, 1, 1) == 0);
  assert(agent_edge_consensus_leader_heartbeat_due(1000, 5200, 4200, 1, 1) == 1);
  assert(agent_edge_consensus_leader_heartbeat_due(1000, 5200, 4200, 0, 1) == 0);
  assert(agent_edge_consensus_leader_heartbeat_due(1000, 5200, 4200, 1, 0) == 0);
  assert(agent_edge_consensus_leader_heartbeat_due(0, 5200, 4200, 1, 1) == 0);

  assert(agent_edge_consensus_leader_lease_expired(3000, 9000, 6000, 1, 0) == 1);
  assert(agent_edge_consensus_leader_lease_expired(3000, 8999, 6000, 1, 0) == 0);
  assert(agent_edge_consensus_leader_lease_expired(3000, 9000, 6000, 0, 0) == 0);
  assert(agent_edge_consensus_leader_lease_expired(3000, 9000, 6000, 1, 1) == 0);
  assert(agent_edge_consensus_leader_lease_expired(3000, 9000, 0, 1, 0) == 0);

  assert(agent_edge_consensus_lease_expiry_recampaign_delay_active(2000, 7000, 6000) == 1);
  assert(agent_edge_consensus_lease_expiry_recampaign_delay_active(2000, 8000, 6000) == 0);
  assert(agent_edge_consensus_lease_expiry_recampaign_delay_active(2000, 7000, 0) == 0);
  assert(agent_edge_consensus_lease_expiry_recampaign_delay_active(0, 7000, 6000) == 0);

  assert(agent_edge_consensus_campaign_start_due(1000, 1000, 500, 0, 0, 0) == 0);
  assert(agent_edge_consensus_campaign_start_due(1500, 1000, 500, 0, 0, 0) == 1);
  assert(agent_edge_consensus_campaign_start_due(1500, 0, 500, 0, 0, 0) == 0);
  assert(agent_edge_consensus_campaign_start_due(1500, 0, 0, 0, 0, 0) == 1);
  assert(agent_edge_consensus_campaign_start_due(5000, 1000, 500, 1, 4500, 1000) == 0);
  assert(agent_edge_consensus_campaign_start_due(5500, 1000, 500, 1, 4500, 1000) == 1);
  assert(agent_edge_consensus_campaign_start_due(5500, 1000, 500, 1, 0, 1000) == 1);
  assert(agent_edge_consensus_campaign_start_due(5500, 1000, 500, 1, 4500, 0) == 0);
}

void test_edge_interop_module(void) {
  test_id_is_safe_basic();
  test_trace_id_is_safe_allows_at();
  test_sha256_token_is_safe_basic();
  test_sanitize_trims_and_defaults();
  test_sanitize_respects_max_len();
  test_result_attest_signing_input_v0_1();
  test_consensus_constants_and_quorum();
  test_consensus_policy_timing_normalize();
  test_consensus_loop_timing_gates();
}
