import { z } from "zod";

export const AgentdTraceRespSchema = z
  .object({
    ok: z.boolean(),
    trace_id: z.string().optional(),
    count: z.number().optional(),
    records: z.array(z.any()).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type AgentdTraceResp = z.infer<typeof AgentdTraceRespSchema>;

export const BrokerTraceRespSchema = z
  .object({
    ok: z.boolean(),
    trace_id: z.string().optional(),
    orchestrate: z.array(z.any()).optional(),
    relay_audit: z.array(z.any()).optional(),
    agentd: z.array(z.any()).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTraceResp = z.infer<typeof BrokerTraceRespSchema>;
