import { daemonFetchInit, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import {
  MemoryCheckpointsRespSchema,
  type MemoryCheckpointsResp,
  MemoryCorrelateRespSchema,
  type MemoryCorrelateResp,
  MemoryCorrelationIndexRespSchema,
  type MemoryCorrelationIndexResp,
  MemoryIndexRespSchema,
  type MemoryIndexResp,
  MemoryQueryRespSchema,
  type MemoryQueryResp,
  MemoryRecapsRespSchema,
  type MemoryRecapsResp,
  MemoryRetentionRespSchema,
  type MemoryRetentionResp,
  MemorySalienceRespSchema,
  type MemorySalienceResp,
} from "./schemas/memory";

type MemoryMutationBody = Record<string, unknown>;

export type MemoryQueryParams = {
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  keyPrefix?: string;
  limit?: number;
};

export type MemoryCorrelateParams = {
  traceId: string;
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  keyPrefix?: string;
  maxEntries?: number;
  timeline?: boolean;
};

export type MemoryCheckpointsParams = {
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  limit?: number;
};

export type MemoryIndexParams = {
  sessionId?: string;
  includeStructured?: boolean;
  includeCore?: boolean;
  includeDaily?: boolean;
  includeSession?: boolean;
  dailyDays?: number;
};

export type MemorySalienceParams = {
  includeStructured?: boolean;
  includeDaily?: boolean;
  dailyDays?: number;
  maxItems?: number;
  maxStructuredItems?: number;
  maxDailyItems?: number;
  halfLifeDays?: number;
  importanceWeight?: number;
};

export type MemoryRecapsListParams = {
  limit?: number;
  includeSummary?: boolean;
  kind?: string;
};

export async function apiMemoryQuery(
  base: string,
  params: MemoryQueryParams,
  auth?: ApiAuth,
): Promise<MemoryQueryResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "key_prefix", params.keyPrefix);
  addQueryParam(qs, "limit", params.limit);
  const url = qs.toString() ? `${base}/api/v1/memory/query?${qs.toString()}` : `${base}/api/v1/memory/query`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemoryQueryRespSchema.parse(j);
}

export async function apiMemoryCorrelate(
  base: string,
  params: MemoryCorrelateParams,
  auth?: ApiAuth,
): Promise<MemoryCorrelateResp> {
  const tid = String(params.traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "trace_id", tid);
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "key_prefix", params.keyPrefix);
  addQueryParam(qs, "max_entries", params.maxEntries);
  if (params.timeline) addQueryParam(qs, "timeline", "1");
  const url = `${base}/api/v1/memory/correlate?${qs.toString()}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemoryCorrelateRespSchema.parse(j);
}

export async function apiMemoryCorrelationIndexBuild(
  base: string,
  body: MemoryMutationBody,
  auth?: ApiAuth,
): Promise<MemoryCorrelationIndexResp> {
  const r = await fetch(
    `${base}/api/v1/memory/correlation/index`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return MemoryCorrelationIndexRespSchema.parse(j);
}

export async function apiMemoryCheckpoints(
  base: string,
  params: MemoryCheckpointsParams,
  auth?: ApiAuth,
): Promise<MemoryCheckpointsResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "limit", params.limit);
  const url = qs.toString() ? `${base}/api/v1/memory/checkpoints?${qs.toString()}` : `${base}/api/v1/memory/checkpoints`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemoryCheckpointsRespSchema.parse(j);
}

export async function apiMemoryIndex(
  base: string,
  params: MemoryIndexParams,
  auth?: ApiAuth,
): Promise<MemoryIndexResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "session_id", params.sessionId);
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_core", params.includeCore);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "include_session", params.includeSession);
  addQueryParam(qs, "daily_days", params.dailyDays);
  const url = qs.toString() ? `${base}/api/v1/memory/index?${qs.toString()}` : `${base}/api/v1/memory/index`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemoryIndexRespSchema.parse(j);
}

export async function apiMemorySalience(
  base: string,
  params: MemorySalienceParams,
  auth?: ApiAuth,
): Promise<MemorySalienceResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "daily_days", params.dailyDays);
  addQueryParam(qs, "max_items", params.maxItems);
  addQueryParam(qs, "max_structured_items", params.maxStructuredItems);
  addQueryParam(qs, "max_daily_items", params.maxDailyItems);
  addQueryParam(qs, "half_life_days", params.halfLifeDays);
  addQueryParam(qs, "importance_weight", params.importanceWeight);
  const url = qs.toString() ? `${base}/api/v1/memory/salience?${qs.toString()}` : `${base}/api/v1/memory/salience`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemorySalienceRespSchema.parse(j);
}

export async function apiMemoryRetentionEnforce(
  base: string,
  body: MemoryMutationBody,
  auth?: ApiAuth,
): Promise<MemoryRetentionResp> {
  const r = await fetch(
    `${base}/api/v1/memory/retention/enforce`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return MemoryRetentionRespSchema.parse(j);
}

export async function apiMemoryRecapsList(
  base: string,
  params: MemoryRecapsListParams,
  auth?: ApiAuth,
): Promise<MemoryRecapsResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "include_summary", params.includeSummary);
  addQueryParam(qs, "kind", params.kind);
  const url = qs.toString() ? `${base}/api/v1/memory/recaps?${qs.toString()}` : `${base}/api/v1/memory/recaps`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return MemoryRecapsRespSchema.parse(j);
}

export async function apiMemoryRecapsCreate(
  base: string,
  body: MemoryMutationBody,
  auth?: ApiAuth,
): Promise<MemoryRecapsResp> {
  const r = await fetch(
    `${base}/api/v1/memory/recaps`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return MemoryRecapsRespSchema.parse(j);
}
