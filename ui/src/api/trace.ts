import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  AgentdTraceRespSchema,
  type AgentdTraceResp,
  BrokerTraceRespSchema,
  type BrokerTraceResp,
} from "./schemas/trace";

export async function apiAgentdTrace(base: string, traceId: string, auth?: ApiAuth): Promise<AgentdTraceResp> {
  const tid = String(traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const r = await fetch(
    `${base}/api/v1/trace?trace_id=${encodeURIComponent(tid)}&limit=200&max_bytes=1048576`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return AgentdTraceRespSchema.parse(j);
}

export async function apiBrokerTrace(brokerBase: string, traceId: string, auth?: ApiAuth): Promise<BrokerTraceResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const r = await fetch(`${base}/v1/trace?trace_id=${encodeURIComponent(tid)}&limit=200&fanout=1`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerTraceRespSchema.parse(j);
}
