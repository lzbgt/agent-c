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
