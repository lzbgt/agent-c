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

export const BrokerTeamRunRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.string().optional(),
    created_unix_ms: z.number().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunResp = z.infer<typeof BrokerTeamRunRespSchema>;

export const BrokerTeamRunStatusRespSchema = z
  .object({
    ok: z.boolean(),
    team_id: z.string().optional(),
    team_run_id: z.string().optional(),
    status: z.string().optional(),
    created_unix_ms: z.number().optional(),
    members: z.array(BrokerTeamMemberSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTeamRunStatusResp = z.infer<typeof BrokerTeamRunStatusRespSchema>;

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
