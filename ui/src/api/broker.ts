import { daemonHeaders, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import {
  BrokerAgentsRespSchema,
  type BrokerAgentsResp,
  BrokerDeploymentsRespSchema,
  type BrokerDeploymentsResp,
  BrokerMembersRespSchema,
  type BrokerMembersResp,
  BrokerMembershipAuditRespSchema,
  type BrokerMembershipAuditResp,
} from "./schemas/broker";
import type { MemoryRecapsListParams, MemorySalienceParams } from "./memory";

export async function apiBrokerListAgents(brokerBase: string, auth?: ApiAuth): Promise<BrokerAgentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/agents`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerAgentsRespSchema.parse(j);
}

export async function apiBrokerListDeployments(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
): Promise<BrokerDeploymentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/deployments`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerDeploymentsRespSchema.parse(j);
}

export async function apiBrokerProxyJson(
  brokerBase: string,
  agentId: string,
  path: string,
  method: string,
  body: unknown,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const p = path.startsWith("/") ? path : `/${path}`;
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const dep = typeof deploymentId === "string" ? deploymentId.trim() : "";
  if (dep) headers["X-Agentd-Deployment"] = dep;
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/proxy${p}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerOtaUpdate(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/update", "POST", body, auth, deploymentId);
}

export async function apiBrokerOtaUpdateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...body };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/ota/update`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerOtaStatus(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/status", "GET", undefined, auth, deploymentId);
}

export async function apiBrokerOtaStatusBulk(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const params = new URLSearchParams();
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    params.set("deployment_ids", deploymentIds.join(","));
  }
  const qs = params.toString();
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/ota/status${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRetentionBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...(body ?? {}) };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/memory/retention/enforce`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsListBulk(
  brokerBase: string,
  agentId: string,
  params: MemoryRecapsListParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "include_summary", params.includeSummary);
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    qs.set("deployment_ids", deploymentIds.join(","));
  }
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/memory/recaps${qs.toString() ? `?${qs.toString()}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemorySalienceBulk(
  brokerBase: string,
  agentId: string,
  params: MemorySalienceParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "daily_days", params.dailyDays);
  addQueryParam(qs, "max_items", params.maxItems);
  addQueryParam(qs, "max_structured_items", params.maxStructuredItems);
  addQueryParam(qs, "max_daily_items", params.maxDailyItems);
  addQueryParam(qs, "half_life_days", params.halfLifeDays);
  addQueryParam(qs, "importance_weight", params.importanceWeight);
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    qs.set("deployment_ids", deploymentIds.join(","));
  }
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/memory/salience${qs.toString() ? `?${qs.toString()}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsCreateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...(body ?? {}) };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/memory/recaps`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerGetMembers(brokerBase: string, agentId: string, auth?: ApiAuth): Promise<BrokerMembersResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerMembersRespSchema.parse(j);
}

export async function apiBrokerUpsertMember(
  brokerBase: string,
  agentId: string,
  req: { user_sub: string; role?: string },
  auth?: ApiAuth,
): Promise<{ ok: boolean; error?: string }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify({
      user_sub: String(req?.user_sub || "").trim(),
      role: String(req?.role || "").trim(),
    }),
  });
  const j = await r.json();
  if (!j || typeof j !== "object") throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: String((j as any).error || (j as any).err || "request failed") };
}

export async function apiBrokerDeleteMember(
  brokerBase: string,
  agentId: string,
  userSub: string,
  auth?: ApiAuth,
): Promise<{ ok: boolean; error?: string }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const sub = String(userSub || "").trim();
  if (!sub) throw new Error("missing user_sub");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members/${encodeURIComponent(sub)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  if (!j || typeof j !== "object") throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: String((j as any).error || (j as any).err || "request failed") };
}

export async function apiBrokerGetMembershipAudit(
  brokerBase: string,
  agentId: string,
  limit: number,
  auth?: ApiAuth,
): Promise<BrokerMembershipAuditResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const lim = Number.isFinite(limit) ? Math.max(1, Math.min(limit, 500)) : 200;
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/membership_audit?limit=${encodeURIComponent(String(lim))}`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerMembershipAuditRespSchema.parse(j);
}
