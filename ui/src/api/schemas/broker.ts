import { z } from "zod";

export const BrokerAgentsRespSchema = z
  .object({
    ok: z.boolean(),
    agents: z
      .array(
        z
          .object({
            agent_id: z.string(),
            enabled: z.boolean().optional(),
            created_unix_ms: z.number().optional(),
            owner_sub: z.string().optional(),
            connected: z.boolean().optional(),
            connected_unix_ms: z.number().optional(),
            last_seen_unix_ms: z.number().optional(),
            remote_addr: z.string().optional(),
            labels: z.record(z.any()).optional(),
            meta: z.record(z.any()).optional(),
            deployments: z
              .array(
                z
                  .object({
                    deployment_id: z.string().optional(),
                    connected: z.boolean().optional(),
                    connected_unix_ms: z.number().optional(),
                    last_seen_unix_ms: z.number().optional(),
                    remote_addr: z.string().optional(),
                    meta: z.record(z.any()).optional(),
                  })
                  .passthrough(),
              )
              .optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerAgentsResp = z.infer<typeof BrokerAgentsRespSchema>;

export const BrokerDeploymentsRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    default_deployment_id: z.string().optional(),
    deployments: z
      .array(
        z
          .object({
            deployment_id: z.string().optional(),
            connected: z.boolean().optional(),
            connected_unix_ms: z.number().optional(),
            last_seen_unix_ms: z.number().optional(),
            remote_addr: z.string().optional(),
            meta: z.record(z.any()).optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerDeploymentsResp = z.infer<typeof BrokerDeploymentsRespSchema>;

export const BrokerMembersRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    owner_sub: z.string().optional(),
    members: z
      .array(
        z
          .object({
            user_sub: z.string(),
            role: z.string().optional(),
            created_unix_ms: z.number().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerMembersResp = z.infer<typeof BrokerMembersRespSchema>;

export const BrokerMembershipAuditRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    owner_sub: z.string().optional(),
    audit: z
      .array(
        z
          .object({
            ts_unix_ms: z.number().optional(),
            actor_sub: z.string().optional(),
            target_sub: z.string().optional(),
            action: z.string().optional(),
            role: z.string().optional(),
            trace_id: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerMembershipAuditResp = z.infer<typeof BrokerMembershipAuditRespSchema>;

export const BrokerEventSchema = z
  .object({
    type: z.string(),
    ts_unix_ms: z.number().optional(),
    agent_id: z.string().optional(),
    user_sub: z.string().optional(),
    event_id: z.string().optional(),
    trace_id: z.string().optional(),
    payload: z.record(z.any()).optional(),
    extra: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerEvent = z.infer<typeof BrokerEventSchema>;

export const BrokerEventsReplayRespSchema = z
  .object({
    ok: z.boolean(),
    user_sub: z.string().optional(),
    since_ts: z.number().optional(),
    next_since_ts: z.number().optional(),
    limit: z.number().optional(),
    count: z.number().optional(),
    events: z.array(BrokerEventSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerEventsReplayResp = z.infer<typeof BrokerEventsReplayRespSchema>;

export const BrokerOrchestratorRunSchema = z
  .object({
    orchestrator_run_id: z.string(),
    team_id: z.string().optional(),
    status: z.string().optional(),
    goal: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().optional(),
    updated_unix_ms: z.number().optional(),
    last_heartbeat_unix_ms: z.number().optional(),
    heartbeat_age_ms: z.number().optional(),
    lease_timeout_ms: z.number().optional(),
    lease_status: z.string().optional(),
    goal_contract: z.record(z.any()).optional(),
    role_plan_snapshot: z.record(z.any()).optional(),
    meta: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerOrchestratorRun = z.infer<typeof BrokerOrchestratorRunSchema>;

export const BrokerOrchestratorRunRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    run: BrokerOrchestratorRunSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerOrchestratorRunResp = z.infer<typeof BrokerOrchestratorRunRespSchema>;

export const BrokerOrchestratorRunListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    runs: z.array(BrokerOrchestratorRunSchema).optional(),
    limit: z.number().optional(),
    offset: z.number().optional(),
    status: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerOrchestratorRunListResp = z.infer<typeof BrokerOrchestratorRunListRespSchema>;

export const BrokerOrchestratorSpawnRequestSchema = z
  .object({
    spawn_request_id: z.string(),
    team_id: z.string().optional(),
    orchestrator_run_id: z.string().optional(),
    role: z.string().optional(),
    count: z.number().optional(),
    status: z.string().optional(),
    requirements: z.record(z.any()).optional(),
    assigned_members: z.array(z.record(z.any())).optional(),
    error: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().optional(),
    updated_unix_ms: z.number().optional(),
    meta: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerOrchestratorSpawnRequest = z.infer<typeof BrokerOrchestratorSpawnRequestSchema>;

export const BrokerOrchestratorSpawnRequestRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    spawn_request: BrokerOrchestratorSpawnRequestSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerOrchestratorSpawnRequestResp = z.infer<typeof BrokerOrchestratorSpawnRequestRespSchema>;

export const BrokerOrchestratorSpawnRequestListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    spawn_requests: z.array(BrokerOrchestratorSpawnRequestSchema).optional(),
    limit: z.number().optional(),
    offset: z.number().optional(),
    status: z.string().optional(),
    orchestrator_run_id: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerOrchestratorSpawnRequestListResp = z.infer<typeof BrokerOrchestratorSpawnRequestListRespSchema>;

export const BrokerTeamSchema = z
  .object({
    team_id: z.string(),
    owner_sub: z.string().optional(),
    display_name: z.string().optional(),
    created_unix_ms: z.number().optional(),
    tags: z.array(z.string()).optional(),
    policy_ref: z.string().optional(),
    shared_memory_scope_id: z.string().optional(),
    meta: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerTeam = z.infer<typeof BrokerTeamSchema>;

export const BrokerTeamListRespSchema = z
  .object({
    ok: z.boolean(),
    teams: z.array(BrokerTeamSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamListResp = z.infer<typeof BrokerTeamListRespSchema>;

export const BrokerTeamCreateRespSchema = z
  .object({
    ok: z.boolean(),
    team: BrokerTeamSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamCreateResp = z.infer<typeof BrokerTeamCreateRespSchema>;

export const BrokerTeamGetRespSchema = z
  .object({
    ok: z.boolean(),
    team: BrokerTeamSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamGetResp = z.infer<typeof BrokerTeamGetRespSchema>;

export const BrokerTeamDeleteRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamDeleteResp = z.infer<typeof BrokerTeamDeleteRespSchema>;

export const BrokerTeamMemberSchema = z
  .object({
    member_id: z.string(),
    team_id: z.string().optional(),
    deployment_id: z.string().optional(),
    agent_id: z.string().optional(),
    role: z.string().optional(),
    capabilities: z.array(z.string()).optional(),
    status: z.string().optional(),
    weight: z.number().optional(),
    created_unix_ms: z.number().optional(),
    meta: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerTeamMember = z.infer<typeof BrokerTeamMemberSchema>;

export const BrokerTeamMemberListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    members: z.array(BrokerTeamMemberSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamMemberListResp = z.infer<typeof BrokerTeamMemberListRespSchema>;

export const BrokerTeamMemberUpsertRespSchema = z
  .object({
    ok: z.boolean(),
    member: BrokerTeamMemberSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamMemberUpsertResp = z.infer<typeof BrokerTeamMemberUpsertRespSchema>;

export const BrokerTeamQuorumRuleSchema = z
  .object({
    rule_id: z.string(),
    team_id: z.string().optional(),
    action: z.string().optional(),
    tool_names: z.array(z.string()).optional(),
    min_approvals: z.number().optional(),
    role_allowlist: z.array(z.string()).optional(),
    require_distinct_roles: z.boolean().optional(),
    timeout_ms: z.number().optional(),
    quorum_mode: z.string().optional(),
    created_unix_ms: z.number().optional(),
    meta: z.record(z.any()).optional(),
  })
  .passthrough();
export type BrokerTeamQuorumRule = z.infer<typeof BrokerTeamQuorumRuleSchema>;

export const BrokerTeamQuorumRuleListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    rules: z.array(BrokerTeamQuorumRuleSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamQuorumRuleListResp = z.infer<typeof BrokerTeamQuorumRuleListRespSchema>;

export const BrokerTeamQuorumRuleUpsertRespSchema = z
  .object({
    ok: z.boolean(),
    rule: BrokerTeamQuorumRuleSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamQuorumRuleUpsertResp = z.infer<typeof BrokerTeamQuorumRuleUpsertRespSchema>;

export const BrokerTeamRunMemberJobSchema = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    job_id: z.string().optional(),
    status: z.string().optional(),
    ok: z.boolean().optional(),
    error: z.string().optional(),
    http_status: z.number().optional(),
    updated_unix_ms: z.number().optional(),
    dispatch_error: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunMemberJobSummarySchema = z
  .object({
    total: z.number().optional(),
    queued: z.number().optional(),
    running: z.number().optional(),
    done: z.number().optional(),
    error: z.number().optional(),
    cancelled: z.number().optional(),
    interrupted: z.number().optional(),
    unknown: z.number().optional(),
    ok: z.number().optional(),
    failed: z.number().optional(),
    dispatch_errors: z.number().optional(),
  })
  .passthrough();

export const BrokerTeamRunCancelResultSchema = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    job_id: z.string().optional(),
    ok: z.boolean().optional(),
    error: z.string().optional(),
    http_status: z.number().optional(),
  })
  .passthrough();

export const BrokerTeamRunDispatchErrorSchema = z
  .object({
    member_id: z.string().optional(),
    agent_id: z.string().optional(),
    error: z.string().optional(),
  })
  .passthrough();

export const BrokerTeamRunRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.string().optional(),
    mode: z.string().optional(),
    created_unix_ms: z.number().optional(),
    member_jobs: z.array(BrokerTeamRunMemberJobSchema).optional(),
    dispatch_errors: z.array(BrokerTeamRunDispatchErrorSchema).optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunResp = z.infer<typeof BrokerTeamRunRespSchema>;

export const BrokerTeamRunSummarySchema = z
  .object({
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.string().optional(),
    mode: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
  })
  .passthrough();
export type BrokerTeamRunSummary = z.infer<typeof BrokerTeamRunSummarySchema>;

export const BrokerTeamRunGoalContractSchema = z
  .object({
    goal: z.string().optional(),
    success_criteria: z.array(z.string()).optional(),
    constraints: z.array(z.string()).optional(),
  })
  .passthrough();

export const BrokerTeamRunGoalEventSchema = z
  .object({
    type: z.string().optional(),
    ts_unix_ms: z.number().optional(),
    message: z.string().optional(),
    event_index: z.number().optional(),
    data: z.record(z.any()).optional(),
  })
  .passthrough();

export const BrokerTeamRunHandoffEventSchema = z
  .object({
    from_role: z.string().optional(),
    to_role: z.string().optional(),
    reason: z.string().optional(),
    message: z.string().optional(),
    ts_unix_ms: z.number().optional(),
    event_index: z.number().optional(),
    data: z.record(z.any()).optional(),
  })
  .passthrough();

export const BrokerTeamRunListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    limit: z.number().optional(),
    offset: z.number().optional(),
    status: z.string().optional(),
    runs: z.array(BrokerTeamRunSummarySchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunListResp = z.infer<typeof BrokerTeamRunListRespSchema>;

export const BrokerTeamRunStatusRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.string().optional(),
    mode: z.string().optional(),
    created_unix_ms: z.number().optional(),
    members: z.array(BrokerTeamMemberSchema).optional(),
    member_sessions: z.record(z.string()).optional(),
    member_jobs: z.array(BrokerTeamRunMemberJobSchema).optional(),
    dispatch_errors: z.array(BrokerTeamRunDispatchErrorSchema).optional(),
    member_job_summary: BrokerTeamRunMemberJobSummarySchema.optional(),
    cancel_requested_unix_ms: z.number().optional(),
    cancel_results: z.array(BrokerTeamRunCancelResultSchema).optional(),
    run_overrides_mode: z.string().optional(),
    role_overrides_applied: z.record(z.any()).optional(),
    member_overrides_applied: z.record(z.any()).optional(),
    runtime_members: z.array(z.record(z.any())).optional(),
    goal_contract: BrokerTeamRunGoalContractSchema.optional(),
    goal_updated_unix_ms: z.number().optional(),
    goal_events: z.array(BrokerTeamRunGoalEventSchema).optional(),
    goal_events_updated_unix_ms: z.number().optional(),
    handoff_events: z.array(BrokerTeamRunHandoffEventSchema).optional(),
    handoff_events_updated_unix_ms: z.number().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunStatusResp = z.infer<typeof BrokerTeamRunStatusRespSchema>;

export const BrokerTeamRunGoalUpdateRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    goal_contract: BrokerTeamRunGoalContractSchema.optional(),
    goal_events: z.array(BrokerTeamRunGoalEventSchema).optional(),
    goal_event_count: z.number().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunGoalUpdateResp = z.infer<typeof BrokerTeamRunGoalUpdateRespSchema>;

export const BrokerTeamRunHandoffRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    handoff_events: z.array(BrokerTeamRunHandoffEventSchema).optional(),
    handoff_event_count: z.number().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunHandoffResp = z.infer<typeof BrokerTeamRunHandoffRespSchema>;

export const BrokerTeamRunModeratorRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    dispatched: z
      .array(
        z
          .object({
            member_id: z.string().optional(),
            agent_id: z.string().optional(),
            deployment_id: z.string().optional(),
            session_id: z.string().optional(),
            ok: z.boolean().optional(),
            http_status: z.number().optional(),
            error: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    skipped: z
      .array(
        z
          .object({
            member_id: z.string().optional(),
            agent_id: z.string().optional(),
            reason: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunModeratorResp = z.infer<typeof BrokerTeamRunModeratorRespSchema>;

export const BrokerTeamRunModeratorEventsRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    events: z
      .array(
        z
          .object({
            member_id: z.string().optional(),
            agent_id: z.string().optional(),
            deployment_id: z.string().optional(),
            session_id: z.string().optional(),
            type: z.string().optional(),
            ts_unix_ms: z.number().optional(),
            event: z.record(z.any()).optional(),
          })
          .passthrough(),
      )
      .optional(),
    errors: z
      .array(
        z
          .object({
            member_id: z.string().optional(),
            agent_id: z.string().optional(),
            deployment_id: z.string().optional(),
            session_id: z.string().optional(),
            ok: z.boolean().optional(),
            http_status: z.number().optional(),
            error: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    skipped: z
      .array(
        z
          .object({
            member_id: z.string().optional(),
            agent_id: z.string().optional(),
            reason: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunModeratorEventsResp = z.infer<typeof BrokerTeamRunModeratorEventsRespSchema>;

export const BrokerTeamRunApprovalSchema = z
  .object({
    approval_id: z.string().optional(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    rule_id: z.string().optional(),
    member_id: z.string().optional(),
    role: z.string().optional(),
    decision: z.string().optional(),
    reason: z.string().optional(),
    created_by: z.string().optional(),
    created_unix_ms: z.number().optional(),
  })
  .passthrough();
export type BrokerTeamRunApproval = z.infer<typeof BrokerTeamRunApprovalSchema>;

export const BrokerTeamRunApprovalListRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    approvals: z.array(BrokerTeamRunApprovalSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunApprovalListResp = z.infer<typeof BrokerTeamRunApprovalListRespSchema>;
