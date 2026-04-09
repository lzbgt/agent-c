import { z } from "zod";
import type { components as BrokerComponents, paths as BrokerPaths } from "../generated/broker-openapi";

type BrokerErrorFields = {
  error?: string;
  err?: string;
  code?: string;
};

export type BrokerAgentsResp =
  BrokerPaths["/v1/agents"]["get"]["responses"][200]["content"]["application/json"] & BrokerErrorFields;
export type BrokerAuthSessionResp =
  BrokerPaths["/v1/auth/session"]["post"]["responses"][200]["content"]["application/json"] & BrokerErrorFields;
export type BrokerAudioSessionSummary = BrokerComponents["schemas"]["AudioSessionSummary"];
export type BrokerAudioSessionListResp = BrokerComponents["schemas"]["AudioSessionListResponse"] & BrokerErrorFields;
export type BrokerAudioSessionGetResp = BrokerComponents["schemas"]["AudioSessionGetResponse"] & BrokerErrorFields;
export type BrokerAudioSessionDeleteResp = BrokerComponents["schemas"]["AudioSessionDeleteResponse"] & BrokerErrorFields;
export type BrokerAudioSessionCreateResp =
  BrokerPaths["/v1/audio/sessions"]["post"]["responses"][200]["content"]["application/json"] & BrokerErrorFields;
export type BrokerAudioSignalResp =
  BrokerPaths["/v1/audio/sessions/{session_id}/signal"]["post"]["responses"][200]["content"]["application/json"] &
    BrokerErrorFields;
export type BrokerAudioSignalEvent = BrokerComponents["schemas"]["AudioSignalEvent"];
export type BrokerDeploymentsResp = BrokerComponents["schemas"]["AgentDeploymentsResponse"] & BrokerErrorFields;
export type BrokerMembersResp =
  BrokerPaths["/v1/agents/{agent_id}/members"]["get"]["responses"][200]["content"]["application/json"] & BrokerErrorFields;
export type BrokerMembershipAuditResp =
  BrokerPaths["/v1/agents/{agent_id}/membership_audit"]["get"]["responses"][200]["content"]["application/json"] &
    BrokerErrorFields;
export type BrokerConnector = BrokerComponents["schemas"]["ConnectorInfo"];
export type BrokerConnectorsResp = BrokerComponents["schemas"]["ConnectorListResponse"] & BrokerErrorFields;
export type BrokerEvent = BrokerComponents["schemas"]["BrokerEvent"];
export type BrokerEventsReplayResp = BrokerComponents["schemas"]["BrokerEventsReplayResponse"] & BrokerErrorFields;

type AgentInfo = BrokerComponents["schemas"]["AgentInfo"];
type DeploymentInfo = BrokerComponents["schemas"]["DeploymentInfo"];
type AgentMember = BrokerComponents["schemas"]["AgentMember"];
type MembershipAuditRow = BrokerComponents["schemas"]["MembershipAuditRow"];
export type BrokerAgentInfo = AgentInfo;
export type BrokerDeploymentInfo = DeploymentInfo;
export type BrokerTeamRunGoalContract = BrokerComponents["schemas"]["TeamRunGoalContract"];
export type BrokerTeamRunGoalEvent = BrokerComponents["schemas"]["TeamRunGoalEvent"];
export type BrokerTeamRunHandoffEvent = BrokerComponents["schemas"]["TeamRunHandoffEvent"];
export type BrokerOrchestratorRun = BrokerComponents["schemas"]["OrchestratorRun"];
export type BrokerOrchestratorRunCreateRequest = BrokerComponents["schemas"]["OrchestratorRunCreateRequest"];
export type BrokerOrchestratorRunUpdateRequest = BrokerComponents["schemas"]["OrchestratorRunUpdateRequest"];
export type BrokerOrchestratorRunHeartbeatRequest = BrokerComponents["schemas"]["OrchestratorRunHeartbeatRequest"];
export type BrokerOrchestratorRunResp = BrokerComponents["schemas"]["OrchestratorRunResponse"] & BrokerErrorFields;
export type BrokerOrchestratorRunListResp = BrokerComponents["schemas"]["OrchestratorRunListResponse"] & BrokerErrorFields;
export type BrokerOrchestratorSpawnRequest = BrokerComponents["schemas"]["OrchestratorSpawnRequest"];
export type BrokerOrchestratorSpawnRequestCreateRequest =
  BrokerComponents["schemas"]["OrchestratorSpawnRequestCreateRequest"];
export type BrokerOrchestratorSpawnRequestUpdateRequest =
  BrokerComponents["schemas"]["OrchestratorSpawnRequestUpdateRequest"];
export type BrokerOrchestratorSpawnRequestResp =
  BrokerComponents["schemas"]["OrchestratorSpawnRequestResponse"] & BrokerErrorFields;
export type BrokerOrchestratorSpawnRequestListResp =
  BrokerComponents["schemas"]["OrchestratorSpawnRequestListResponse"] & BrokerErrorFields;
export type BrokerTeam = BrokerComponents["schemas"]["Team"];
// Server-side create accepts an optional caller-supplied team_id even though the generated schema currently omits it.
export type BrokerTeamCreateRequest = BrokerComponents["schemas"]["TeamCreateRequest"] & { team_id?: string };
export type BrokerTeamUpdateRequest = BrokerComponents["schemas"]["TeamUpdateRequest"];
export type BrokerTeamListResp = BrokerComponents["schemas"]["TeamListResponse"] & BrokerErrorFields;
export type BrokerTeamCreateResp = BrokerComponents["schemas"]["TeamCreateResponse"] & BrokerErrorFields;
export type BrokerTeamGetResp = BrokerComponents["schemas"]["TeamGetResponse"] & BrokerErrorFields;
export type BrokerTeamDeleteResp = BrokerComponents["schemas"]["TeamDeleteResponse"] & BrokerErrorFields;
export type BrokerTeamMember = BrokerComponents["schemas"]["TeamMember"];
export type BrokerTeamMemberUpsertRequest = BrokerComponents["schemas"]["TeamMemberUpsertRequest"];
export type BrokerTeamMemberUpdateRequest = BrokerComponents["schemas"]["TeamMemberUpdateRequest"];
export type BrokerTeamMemberListResp = BrokerComponents["schemas"]["TeamMemberListResponse"] & BrokerErrorFields;
export type BrokerTeamMemberUpsertResp = BrokerComponents["schemas"]["TeamMemberUpsertResponse"] & BrokerErrorFields;
export type BrokerTeamQuorumRule = BrokerComponents["schemas"]["TeamQuorumRule"];
export type BrokerTeamQuorumRuleUpsertRequest = BrokerComponents["schemas"]["TeamQuorumRuleUpsertRequest"];
export type BrokerTeamQuorumRuleListResp =
  BrokerComponents["schemas"]["TeamQuorumRuleListResponse"] & BrokerErrorFields;
export type BrokerTeamQuorumRuleUpsertResp =
  BrokerComponents["schemas"]["TeamQuorumRuleUpsertResponse"] & BrokerErrorFields;
export type BrokerTeamRunRequest = BrokerComponents["schemas"]["TeamRunRequest"];
export type BrokerTeamRunRuntimeMember = BrokerComponents["schemas"]["TeamRunRuntimeMember"];
export type BrokerTeamRunMemberJob = BrokerComponents["schemas"]["TeamRunMemberJob"];
export type BrokerTeamRunMemberJobSummary = BrokerComponents["schemas"]["TeamRunMemberJobSummary"];
export type BrokerTeamRunCancelResult = BrokerComponents["schemas"]["TeamRunCancelResult"];
export type BrokerTeamRunDispatchError = BrokerComponents["schemas"]["TeamRunDispatchError"];
export type BrokerTeamRunResp = BrokerComponents["schemas"]["TeamRunResponse"] & BrokerErrorFields;
export type BrokerTeamRunSummary = BrokerComponents["schemas"]["TeamRunSummary"];
export type BrokerTeamRunListResp = BrokerComponents["schemas"]["TeamRunListResponse"] & BrokerErrorFields;
export type BrokerTeamRunStatusResp = BrokerComponents["schemas"]["TeamRunStatusResponse"] & {
  member_sessions?: Record<string, string>;
  goal_updated_unix_ms?: number;
  goal_events_updated_unix_ms?: number;
  handoff_events_updated_unix_ms?: number;
} & BrokerErrorFields;
export type BrokerTeamRunGoalUpdateRequest = BrokerComponents["schemas"]["TeamRunGoalUpdateRequest"];
export type BrokerTeamRunGoalUpdateResp = BrokerComponents["schemas"]["TeamRunGoalUpdateResponse"] & BrokerErrorFields;
export type BrokerTeamRunHandoffUpdateRequest = BrokerComponents["schemas"]["TeamRunHandoffUpdateRequest"];
export type BrokerTeamRunHandoffResp = BrokerComponents["schemas"]["TeamRunHandoffUpdateResponse"] & BrokerErrorFields;
export type BrokerTeamRunRuntimeMembersUpdateRequest =
  BrokerComponents["schemas"]["TeamRunRuntimeMembersUpdateRequest"];
export type BrokerTeamRuntimeMembersAllocateRequest = BrokerComponents["schemas"]["TeamRuntimeMembersAllocateRequest"];
export type BrokerTeamRunModeratorDispatch = BrokerComponents["schemas"]["TeamRunModeratorDispatch"];
export type BrokerTeamRunModeratorSkipped = BrokerComponents["schemas"]["TeamRunModeratorSkipped"];
export type BrokerTeamRunModeratorEvent = BrokerComponents["schemas"]["TeamRunModeratorEvent"];
export type BrokerTeamRunModeratorDirectiveRequest =
  BrokerComponents["schemas"]["TeamRunModeratorDirectiveRequest"];
export type BrokerTeamRunModeratorTaskRequest = BrokerComponents["schemas"]["TeamRunModeratorTaskRequest"];
export type BrokerTeamRunModeratorResp = BrokerComponents["schemas"]["TeamRunModeratorResponse"] & BrokerErrorFields;
export type BrokerTeamRunModeratorEventsResp =
  BrokerComponents["schemas"]["TeamRunModeratorEventsResponse"] & BrokerErrorFields;
export type BrokerTeamRunApprovalCreateRequest = BrokerComponents["schemas"]["TeamRunApprovalCreateRequest"];
export type BrokerTeamRunApproval = BrokerComponents["schemas"]["TeamRunApproval"];
export type BrokerTeamRunApprovalListResp =
  BrokerComponents["schemas"]["TeamRunApprovalListResponse"] & BrokerErrorFields;
export type BrokerGuidanceEvent = BrokerComponents["schemas"]["GuidanceEvent"];
export type BrokerGuidanceReceipt = BrokerComponents["schemas"]["GuidanceReceipt"];
export type BrokerGuidanceCreateRequest = BrokerComponents["schemas"]["GuidanceCreateRequest"];
export type BrokerGuidanceAckRequest = BrokerComponents["schemas"]["GuidanceAckRequest"];

const UnknownRecordSchema = z.record(z.string(), z.unknown());
const DeploymentInfoSchema: z.ZodType<DeploymentInfo> = z
  .object({
    deployment_id: z.string(),
    connected: z.boolean(),
    connected_unix_ms: z.number().int().nonnegative().optional(),
    last_seen_unix_ms: z.number().int().nonnegative().optional(),
    remote_addr: z.string().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

const AgentInfoSchema: z.ZodType<AgentInfo> = z
  .object({
    agent_id: z.string(),
    enabled: z.boolean(),
    created_unix_ms: z.number().int().nonnegative(),
    owner_sub: z.string(),
    connected: z.boolean(),
    connected_unix_ms: z.number().int().nonnegative().optional(),
    last_seen_unix_ms: z.number().int().nonnegative().optional(),
    remote_addr: z.string().optional(),
    labels: UnknownRecordSchema.optional(),
    meta: UnknownRecordSchema.optional(),
    deployments: z.array(DeploymentInfoSchema).optional(),
  })
  .passthrough();

const AgentMemberSchema: z.ZodType<AgentMember> = z
  .object({
    user_sub: z.string(),
    role: z.string(),
    created_unix_ms: z.number().int().nonnegative(),
  })
  .passthrough();

const MembershipAuditRowSchema: z.ZodType<MembershipAuditRow> = z
  .object({
    ts_unix_ms: z.number().int().nonnegative(),
    actor_sub: z.string(),
    target_sub: z.string(),
    agent_id: z.string(),
    action: z.string(),
    role: z.string().optional(),
    trace_id: z.string().optional(),
  })
  .passthrough();

export const BrokerAgentsRespSchema: z.ZodType<BrokerAgentsResp> = z
  .object({
    ok: z.boolean(),
    agents: z.array(AgentInfoSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAuthSessionRespSchema: z.ZodType<BrokerAuthSessionResp> = z
  .object({
    ok: z.boolean(),
    cookie_name: z.string(),
    auth_kind: z.string().optional(),
    cleared: z.boolean().optional(),
    http_only: z.boolean(),
    secure: z.boolean(),
    same_site: z.enum(["none", "lax", "strict", "default"]),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSessionSummarySchema: z.ZodType<BrokerAudioSessionSummary> = z
  .object({
    session_id: z.string(),
    agent_id: z.string(),
    deployment_id: z.string().optional(),
    owner_sub: z.string().optional(),
    mode: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative(),
    expires_unix_ms: z.number().int().nonnegative(),
    subscriber_count: z.number().int().nonnegative(),
    signal_count: z.number().int().nonnegative(),
    last_signal_type: z.string().optional(),
    last_signal_from: z.string().optional(),
    last_signal_unix_ms: z.number().int().nonnegative().optional(),
  })
  .passthrough();

export const BrokerAudioSessionListRespSchema: z.ZodType<BrokerAudioSessionListResp> = z
  .object({
    ok: z.boolean(),
    count: z.number().int().nonnegative(),
    sessions: z.array(BrokerAudioSessionSummarySchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSessionGetRespSchema: z.ZodType<BrokerAudioSessionGetResp> = z
  .object({
    ok: z.boolean(),
    session: BrokerAudioSessionSummarySchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSessionDeleteRespSchema: z.ZodType<BrokerAudioSessionDeleteResp> = z
  .object({
    ok: z.boolean(),
    deleted: z.boolean(),
    session_id: z.string(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSessionCreateRespSchema: z.ZodType<BrokerAudioSessionCreateResp> = z
  .object({
    ok: z.boolean(),
    session_id: z.string(),
    expires_unix_ms: z.number().int().nonnegative().optional(),
    signal: z
      .object({
        send_url: z.string().optional(),
        recv_url: z.string().optional(),
      })
      .passthrough()
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSignalRespSchema: z.ZodType<BrokerAudioSignalResp> = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerAudioSignalEventSchema: z.ZodType<BrokerAudioSignalEvent> = z
  .object({
    type: z.string(),
    payload: UnknownRecordSchema.optional(),
    from: z.string().optional(),
    ts_unix_ms: z.number().int().nonnegative(),
  })
  .passthrough();

export const BrokerDeploymentsRespSchema: z.ZodType<BrokerDeploymentsResp> = z
  .object({
    ok: z.boolean(),
    agent_id: z.string(),
    default_deployment_id: z.string().optional(),
    deployments: z.array(DeploymentInfoSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerMembersRespSchema: z.ZodType<BrokerMembersResp> = z
  .object({
    ok: z.boolean(),
    agent_id: z.string(),
    owner_sub: z.string(),
    members: z.array(AgentMemberSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerMembershipAuditRespSchema: z.ZodType<BrokerMembershipAuditResp> = z
  .object({
    ok: z.boolean(),
    agent_id: z.string(),
    owner_sub: z.string(),
    audit: z.array(MembershipAuditRowSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerConnectorSchema: z.ZodType<BrokerConnector> = z
  .object({
    id: z.string(),
    name: z.string().optional(),
    kind: z.string().optional(),
    status: z.string().optional(),
    description: z.string().optional(),
    last_seen_unix_ms: z.number().int().nonnegative().optional(),
    last_error: z.string().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerConnectorsRespSchema: z.ZodType<BrokerConnectorsResp> = z
  .object({
    ok: z.boolean(),
    count: z.number().int().nonnegative().optional(),
    connectors: z.array(BrokerConnectorSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerEventSchema: z.ZodType<BrokerEvent> = z
  .object({
    type: z.string(),
    ts_unix_ms: z.number().int().nonnegative(),
    agent_id: z.string().optional(),
    user_sub: z.string().optional(),
    event_id: z.string().optional(),
    trace_id: z.string().optional(),
    payload: UnknownRecordSchema.optional(),
    extra: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerEventsReplayRespSchema: z.ZodType<BrokerEventsReplayResp> = z
  .object({
    ok: z.boolean(),
    user_sub: z.string().optional(),
    since_ts: z.number().int().nonnegative().optional(),
    next_since_ts: z.number().int().nonnegative().optional(),
    limit: z.number().int().nonnegative().optional(),
    count: z.number().int().nonnegative().optional(),
    events: z.array(BrokerEventSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunGoalContractSchema: z.ZodType<BrokerTeamRunGoalContract> = z
  .object({
    goal: z.string().optional(),
    success_criteria: z.array(z.string()).optional(),
    constraints: z.array(z.string()).optional(),
  })
  .passthrough();

export const BrokerTeamRunGoalEventSchema: z.ZodType<BrokerTeamRunGoalEvent> = z
  .object({
    type: z.string(),
    ts_unix_ms: z.number().int().nonnegative(),
    message: z.string().optional(),
    event_index: z.number().int().nonnegative().optional(),
    data: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamRunHandoffEventSchema: z.ZodType<BrokerTeamRunHandoffEvent> = z
  .object({
    handoff_id: z.string().optional(),
    kind: z.enum(["role", "cross_deployment"]).optional(),
    state: z.enum(["proposed", "accepted", "declined", "cancelled"]).optional(),
    from_role: z.string(),
    to_role: z.string(),
    reason: z.string().optional(),
    message: z.string().optional(),
    source_deployment_id: z.string().optional(),
    source_session_id: z.string().optional(),
    target_deployment_id: z.string().optional(),
    target_session_id: z.string().optional(),
    ts_unix_ms: z.number().int().nonnegative(),
    event_index: z.number().int().nonnegative().optional(),
    data: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerOrchestratorRunSchema: z.ZodType<BrokerOrchestratorRun> = z
  .object({
    orchestrator_run_id: z.string(),
    team_id: z.string(),
    status: z.string(),
    goal: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    updated_unix_ms: z.number().int().nonnegative().optional(),
    last_heartbeat_unix_ms: z.number().int().nonnegative().optional(),
    heartbeat_age_ms: z.number().int().nonnegative().optional(),
    lease_timeout_ms: z.number().int().nonnegative().optional(),
    lease_status: z.enum(["ok", "stale", "missing", "unknown"]).optional(),
    goal_contract: z.lazy(() => BrokerTeamRunGoalContractSchema).optional(),
    role_plan_snapshot: UnknownRecordSchema.optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerOrchestratorRunRespSchema: z.ZodType<BrokerOrchestratorRunResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    run: BrokerOrchestratorRunSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerOrchestratorRunListRespSchema: z.ZodType<BrokerOrchestratorRunListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    runs: z.array(BrokerOrchestratorRunSchema),
    limit: z.number().int().nonnegative().optional(),
    offset: z.number().int().nonnegative().optional(),
    status: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerOrchestratorSpawnRequestSchema: z.ZodType<BrokerOrchestratorSpawnRequest> = z
  .object({
    spawn_request_id: z.string(),
    team_id: z.string(),
    orchestrator_run_id: z.string().optional(),
    role: z.string(),
    count: z.number().int().nonnegative(),
    status: z.string(),
    requirements: UnknownRecordSchema.optional(),
    assigned_members: z.array(UnknownRecordSchema).optional(),
    error: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    updated_unix_ms: z.number().int().nonnegative().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerOrchestratorSpawnRequestRespSchema: z.ZodType<BrokerOrchestratorSpawnRequestResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    spawn_request: BrokerOrchestratorSpawnRequestSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerOrchestratorSpawnRequestListRespSchema: z.ZodType<BrokerOrchestratorSpawnRequestListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    spawn_requests: z.array(BrokerOrchestratorSpawnRequestSchema),
    limit: z.number().int().nonnegative().optional(),
    offset: z.number().int().nonnegative().optional(),
    status: z.string().optional(),
    orchestrator_run_id: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamSchema: z.ZodType<BrokerTeam> = z
  .object({
    team_id: z.string(),
    display_name: z.string(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative(),
    tags: z.array(z.string()).optional(),
    policy_ref: z.string().optional(),
    shared_memory_scope_id: z.string().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamListRespSchema: z.ZodType<BrokerTeamListResp> = z
  .object({
    ok: z.boolean(),
    teams: z.array(BrokerTeamSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamCreateRespSchema: z.ZodType<BrokerTeamCreateResp> = z
  .object({
    ok: z.boolean(),
    team: BrokerTeamSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamGetRespSchema: z.ZodType<BrokerTeamGetResp> = z
  .object({
    ok: z.boolean(),
    team: BrokerTeamSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamDeleteRespSchema: z.ZodType<BrokerTeamDeleteResp> = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamMemberSchema: z.ZodType<BrokerTeamMember> = z
  .object({
    member_id: z.string(),
    team_id: z.string(),
    deployment_id: z.string().optional(),
    agent_id: z.string().optional(),
    role: z.string(),
    capabilities: z.array(z.string()).optional(),
    status: z.string(),
    weight: z.number().int().optional(),
    created_unix_ms: z.number().int().nonnegative(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamMemberListRespSchema: z.ZodType<BrokerTeamMemberListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    members: z.array(BrokerTeamMemberSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamMemberUpsertRespSchema: z.ZodType<BrokerTeamMemberUpsertResp> = z
  .object({
    ok: z.boolean(),
    member: BrokerTeamMemberSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamQuorumRuleSchema: z.ZodType<BrokerTeamQuorumRule> = z
  .object({
    rule_id: z.string(),
    team_id: z.string(),
    action: z.string(),
    tool_names: z.array(z.string()).optional(),
    min_approvals: z.number().int().nonnegative(),
    role_allowlist: z.array(z.string()).optional(),
    require_distinct_roles: z.boolean().optional(),
    timeout_ms: z.number().int().nonnegative().optional(),
    quorum_mode: z.string(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamQuorumRuleListRespSchema: z.ZodType<BrokerTeamQuorumRuleListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    rules: z.array(BrokerTeamQuorumRuleSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamQuorumRuleUpsertRespSchema: z.ZodType<BrokerTeamQuorumRuleUpsertResp> = z
  .object({
    ok: z.boolean(),
    rule: BrokerTeamQuorumRuleSchema,
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunRuntimeMemberSchema: z.ZodType<BrokerTeamRunRuntimeMember> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string(),
    deployment_id: z.string().optional(),
    role: z.string(),
    capabilities: z.array(z.string()).optional(),
    status: z.string().optional(),
    weight: z.number().int().optional(),
    meta: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamRunMemberJobSchema: z.ZodType<BrokerTeamRunMemberJob> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    job_id: z.string().optional(),
    status: z.string().optional(),
    ok: z.boolean().optional(),
    error: z.string().optional(),
    http_status: z.number().int().nonnegative().optional(),
    updated_unix_ms: z.number().int().nonnegative().optional(),
    dispatch_error: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunMemberJobSummarySchema: z.ZodType<BrokerTeamRunMemberJobSummary> = z
  .object({
    total: z.number().int().nonnegative().optional(),
    queued: z.number().int().nonnegative().optional(),
    running: z.number().int().nonnegative().optional(),
    done: z.number().int().nonnegative().optional(),
    error: z.number().int().nonnegative().optional(),
    cancelled: z.number().int().nonnegative().optional(),
    interrupted: z.number().int().nonnegative().optional(),
    unknown: z.number().int().nonnegative().optional(),
    ok: z.number().int().nonnegative().optional(),
    failed: z.number().int().nonnegative().optional(),
    dispatch_errors: z.number().int().nonnegative().optional(),
  })
  .passthrough();

export const BrokerTeamRunCancelResultSchema: z.ZodType<BrokerTeamRunCancelResult> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    job_id: z.string().optional(),
    ok: z.boolean().optional(),
    error: z.string().optional(),
    http_status: z.number().int().nonnegative().optional(),
  })
  .passthrough();

export const BrokerTeamRunDispatchErrorSchema: z.ZodType<BrokerTeamRunDispatchError> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    error: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunRespSchema: z.ZodType<BrokerTeamRunResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    status: z.string(),
    mode: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    member_jobs: z.array(BrokerTeamRunMemberJobSchema).optional(),
    member_sessions: z.record(z.string()).optional(),
    dispatch_errors: z.array(BrokerTeamRunDispatchErrorSchema).optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunSummarySchema: z.ZodType<BrokerTeamRunSummary> = z
  .object({
    team_id: z.string(),
    team_run_id: z.string(),
    status: z.string(),
    mode: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
  })
  .passthrough();

export const BrokerTeamRunListRespSchema: z.ZodType<BrokerTeamRunListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    limit: z.number().int().nonnegative().optional(),
    offset: z.number().int().nonnegative().optional(),
    status: z.string().optional(),
    runs: z.array(BrokerTeamRunSummarySchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunStatusRespSchema: z.ZodType<BrokerTeamRunStatusResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    status: z.string(),
    mode: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative().optional(),
    run_overrides_mode: z.string().optional(),
    shared_memory_scope_id: z.string().optional(),
    shared_memory_mode: z.string().optional(),
    auto_allocate_roles: z.boolean().optional(),
    auto_allocate_allocated_roles: z.array(z.string()).optional(),
    auto_allocate_missing_roles: z.array(z.string()).optional(),
    auto_allocate_warning: z.string().optional(),
    goal_contract: BrokerTeamRunGoalContractSchema.optional(),
    goal_events: z.array(BrokerTeamRunGoalEventSchema).optional(),
    handoff_events: z.array(BrokerTeamRunHandoffEventSchema).optional(),
    role_overrides_applied: UnknownRecordSchema.optional(),
    member_overrides_applied: UnknownRecordSchema.optional(),
    role_graph: UnknownRecordSchema.optional(),
    role_instructions: z.record(z.string(), z.unknown()).optional(),
    role_prompt_mode: z.string().optional(),
    members: z.array(BrokerTeamMemberSchema).optional(),
    member_sessions: z.record(z.string()).optional(),
    runtime_members: z.array(BrokerTeamRunRuntimeMemberSchema).optional(),
    member_jobs: z.array(BrokerTeamRunMemberJobSchema).optional(),
    dispatch_errors: z.array(BrokerTeamRunDispatchErrorSchema).optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
    cancel_requested_unix_ms: z.number().int().nonnegative().optional(),
    cancel_results: z.array(BrokerTeamRunCancelResultSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunGoalUpdateRespSchema: z.ZodType<BrokerTeamRunGoalUpdateResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    goal_contract: BrokerTeamRunGoalContractSchema.optional(),
    goal_events: z.array(BrokerTeamRunGoalEventSchema).optional(),
    goal_event_count: z.number().int().nonnegative().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunHandoffRespSchema: z.ZodType<BrokerTeamRunHandoffResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    handoff_events: z.array(BrokerTeamRunHandoffEventSchema).optional(),
    handoff_event_count: z.number().int().nonnegative().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

const BrokerTeamRunModeratorDispatchSchema: z.ZodType<BrokerTeamRunModeratorDispatch> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    session_id: z.string().optional(),
    ok: z.boolean().optional(),
    http_status: z.number().int().nonnegative().optional(),
    error: z.string().optional(),
  })
  .passthrough();

const BrokerTeamRunModeratorSkippedSchema: z.ZodType<BrokerTeamRunModeratorSkipped> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    reason: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunModeratorRespSchema: z.ZodType<BrokerTeamRunModeratorResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    dispatched: z.array(BrokerTeamRunModeratorDispatchSchema),
    skipped: z.array(BrokerTeamRunModeratorSkippedSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

const BrokerTeamRunModeratorEventSchema: z.ZodType<BrokerTeamRunModeratorEvent> = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    session_id: z.string().optional(),
    type: z.string().optional(),
    ts_unix_ms: z.number().int().nonnegative().optional(),
    event: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const BrokerTeamRunModeratorEventsRespSchema: z.ZodType<BrokerTeamRunModeratorEventsResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    events: z.array(BrokerTeamRunModeratorEventSchema),
    errors: z.array(BrokerTeamRunModeratorDispatchSchema).optional(),
    skipped: z.array(BrokerTeamRunModeratorSkippedSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunApprovalSchema: z.ZodType<BrokerTeamRunApproval> = z
  .object({
    approval_id: z.string(),
    team_id: z.string(),
    team_run_id: z.string(),
    rule_id: z.string(),
    member_id: z.string(),
    role: z.string(),
    decision: z.string(),
    reason: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative(),
  })
  .passthrough();

export const BrokerTeamRunApprovalListRespSchema: z.ZodType<BrokerTeamRunApprovalListResp> = z
  .object({
    ok: z.boolean(),
    team_id: z.string(),
    team_run_id: z.string(),
    approvals: z.array(BrokerTeamRunApprovalSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const BrokerGuidanceEventSchema: z.ZodType<BrokerGuidanceEvent> = z
  .object({
    guidance_id: z.string(),
    team_id: z.string(),
    team_run_id: z.string().optional(),
    kind: z.string(),
    priority: z.string(),
    message: z.string(),
    payload: UnknownRecordSchema.optional(),
    target_roles: z.array(z.string()).optional(),
    target_member_ids: z.array(z.string()).optional(),
    target_agent_ids: z.array(z.string()).optional(),
    target_orchestrator_id: z.string().optional(),
    created_by: z.string().optional(),
    created_sub: z.string().optional(),
    created_unix_ms: z.number().int().nonnegative(),
    expires_unix_ms: z.number().int().nonnegative().optional(),
    status: z.string(),
    acked_by: z.string().optional(),
    acked_unix_ms: z.number().int().nonnegative().optional(),
    ack_note: z.string().optional(),
  })
  .passthrough();

export const BrokerGuidanceReceiptSchema: z.ZodType<BrokerGuidanceReceipt> = z
  .object({
    id: z.number().int().nonnegative(),
    guidance_id: z.string(),
    ack_by: z.string(),
    ack_role: z.string().optional(),
    ack_source: z.string(),
    ack_note: z.string().optional(),
    acked_unix_ms: z.number().int().nonnegative(),
  })
  .passthrough();

export const BrokerGuidanceListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.array(z.string()).optional(),
    since_ts: z.number().optional(),
    limit: z.number().optional(),
    offset: z.number().optional(),
    count: z.number().optional(),
    guidance: z.array(BrokerGuidanceEventSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerGuidanceListResp = z.infer<typeof BrokerGuidanceListRespSchema>;

export const BrokerGuidanceCreateRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    guidance: BrokerGuidanceEventSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerGuidanceCreateResp = z.infer<typeof BrokerGuidanceCreateRespSchema>;

export const BrokerGuidanceAckRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    guidance: BrokerGuidanceEventSchema.optional(),
    receipt: BrokerGuidanceReceiptSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerGuidanceAckResp = z.infer<typeof BrokerGuidanceAckRespSchema>;

export const BrokerGuidanceReceiptListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    guidance_id: z.string().optional(),
    limit: z.number().optional(),
    count: z.number().optional(),
    receipts: z.array(BrokerGuidanceReceiptSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerGuidanceReceiptListResp = z.infer<typeof BrokerGuidanceReceiptListRespSchema>;
