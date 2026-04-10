import { z } from "zod";
import { EventSchema } from "./run";

const TraceUnknownRecordSchema = z.record(z.string(), z.unknown());
export type TraceUnknownRecord = z.infer<typeof TraceUnknownRecordSchema>;

export const AgentdTraceRecordSchema = z
  .object({
    ts_unix_ms: z.number().optional(),
    session_id: z.string().optional(),
    ok: z.boolean().optional(),
    trace_id: z.string().optional(),
    prompt: z.string().optional(),
    assistant_text: z.string().optional(),
    events: z.array(EventSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type AgentdTraceRecord = z.infer<typeof AgentdTraceRecordSchema>;

export const BrokerTraceOrchestrateRowSchema = z
  .object({
    ts_unix_ms: z.number(),
    trace_id: z.string(),
    request_json: TraceUnknownRecordSchema,
    response_json: TraceUnknownRecordSchema,
  })
  .passthrough();
export type BrokerTraceOrchestrateRow = z.infer<typeof BrokerTraceOrchestrateRowSchema>;

export const BrokerTraceRelayAuditRowSchema = z
  .object({
    ts_unix_ms: z.number().optional(),
    agent_id: z.string().optional(),
    method: z.string().optional(),
    path: z.string().optional(),
    status: z.number().optional(),
    latency_ms: z.number().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerTraceRelayAuditRow = z.infer<typeof BrokerTraceRelayAuditRowSchema>;

export const BrokerTraceAgentdBodySchema = z
  .object({
    ok: z.boolean().optional(),
    trace_id: z.string().optional(),
    count: z.number().optional(),
    records: z.array(AgentdTraceRecordSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTraceAgentdBody = z.infer<typeof BrokerTraceAgentdBodySchema>;

export const BrokerTraceAgentdFanoutRowSchema = z
  .object({
    agent_id: z.string().optional(),
    ok: z.boolean().optional(),
    ms: z.number().optional(),
    http_status: z.number().optional(),
    agent_status: z.number().optional(),
    error: z.string().optional(),
    body: BrokerTraceAgentdBodySchema.optional(),
  })
  .passthrough();
export type BrokerTraceAgentdFanoutRow = z.infer<typeof BrokerTraceAgentdFanoutRowSchema>;

export const AgentdTraceRespSchema = z
  .object({
    ok: z.boolean(),
    trace_id: z.string().optional(),
    count: z.number().optional(),
    records: z.array(AgentdTraceRecordSchema).optional(),
    memory_correlate: z.unknown().optional(),
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
    orchestrate: z.array(BrokerTraceOrchestrateRowSchema).optional(),
    relay_audit: z.array(BrokerTraceRelayAuditRowSchema).optional(),
    membership: z.array(z.unknown()).optional(),
    agentd: z.array(BrokerTraceAgentdFanoutRowSchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type BrokerTraceResp = z.infer<typeof BrokerTraceRespSchema>;
