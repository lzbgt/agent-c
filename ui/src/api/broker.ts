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
  BrokerTeamCreateRespSchema,
  type BrokerTeamCreateResp,
  BrokerTeamDeleteRespSchema,
  type BrokerTeamDeleteResp,
  BrokerTeamGetRespSchema,
  type BrokerTeamGetResp,
  BrokerTeamListRespSchema,
  type BrokerTeamListResp,
  BrokerTeamMemberListRespSchema,
  type BrokerTeamMemberListResp,
  BrokerTeamMemberUpsertRespSchema,
  type BrokerTeamMemberUpsertResp,
  BrokerTeamQuorumRuleListRespSchema,
  type BrokerTeamQuorumRuleListResp,
  BrokerTeamQuorumRuleUpsertRespSchema,
  type BrokerTeamQuorumRuleUpsertResp,
  BrokerTeamRunRespSchema,
  type BrokerTeamRunResp,
  BrokerTeamRunStatusRespSchema,
  type BrokerTeamRunStatusResp,
  BrokerTeamRunApprovalListRespSchema,
  type BrokerTeamRunApprovalListResp,
} from "./schemas/broker";
import {
  ClientPrefsSchema,
  type ClientPrefs,
  ClientPrefsUpdateReqSchema,
  type ClientPrefsUpdateReq,
} from "./schemas/daemon";
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
  addQueryParam(qs, "kind", params.kind);
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

export async function apiBrokerGetClientPrefs(
  brokerBase: string,
  clientId: string,
  clientKind: string,
  auth?: ApiAuth,
): Promise<ClientPrefs> {
  const base = brokerBase.replace(/\/+$/, "");
  const qs = new URLSearchParams({ client_id: clientId, client_kind: clientKind });
  const r = await fetch(`${base}/v1/client_prefs?${qs.toString()}`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return ClientPrefsSchema.parse(j);
}

export async function apiBrokerPostClientPrefs(
  brokerBase: string,
  req: ClientPrefsUpdateReq,
  auth?: ApiAuth,
): Promise<ClientPrefs> {
  const base = brokerBase.replace(/\/+$/, "");
  const payload = ClientPrefsUpdateReqSchema.parse(req);
  const r = await fetch(`${base}/v1/client_prefs`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return ClientPrefsSchema.parse(j);
}

export async function apiBrokerTeamList(brokerBase: string, auth?: ApiAuth): Promise<BrokerTeamListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/teams`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerTeamListRespSchema.parse(j);
}

export async function apiBrokerTeamCreate(
  brokerBase: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamCreateResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const payload = body ?? {};
  const r = await fetch(`${base}/v1/teams`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return BrokerTeamCreateRespSchema.parse(j);
}

export async function apiBrokerTeamUpdate(
  brokerBase: string,
  teamId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamGetResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(teamId || "").trim();
  if (!id) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(id)}`, {
    method: "PATCH",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(body ?? {}),
  });
  const j = await r.json();
  return BrokerTeamGetRespSchema.parse(j);
}

export async function apiBrokerTeamGet(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamGetResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerTeamGetRespSchema.parse(j);
}

export async function apiBrokerTeamDelete(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamDeleteResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamDeleteRespSchema.parse(j);
}

export async function apiBrokerTeamMembersList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamMemberListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/members`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerTeamMemberListRespSchema.parse(j);
}

export async function apiBrokerTeamMembersUpsert(
  brokerBase: string,
  teamId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamMemberUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/members`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return BrokerTeamMemberUpsertRespSchema.parse(j);
}

export async function apiBrokerTeamMemberUpdate(
  brokerBase: string,
  teamId: string,
  memberId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamMemberUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const mid = String(memberId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!mid) throw new Error("missing member_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/members/${encodeURIComponent(mid)}`, {
    method: "PATCH",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(body ?? {}),
  });
  const j = await r.json();
  return BrokerTeamMemberUpsertRespSchema.parse(j);
}

export async function apiBrokerTeamMembersDelete(
  brokerBase: string,
  teamId: string,
  memberId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamDeleteResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const mid = String(memberId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!mid) throw new Error("missing member_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/members/${encodeURIComponent(mid)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamDeleteRespSchema.parse(j);
}

export async function apiBrokerTeamQuorumList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamQuorumRuleListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/quorum`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerTeamQuorumRuleListRespSchema.parse(j);
}

export async function apiBrokerTeamQuorumUpsert(
  brokerBase: string,
  teamId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamQuorumRuleUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/quorum`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return BrokerTeamQuorumRuleUpsertRespSchema.parse(j);
}

export async function apiBrokerTeamQuorumDelete(
  brokerBase: string,
  teamId: string,
  ruleId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamDeleteResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(ruleId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing rule_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/quorum/${encodeURIComponent(rid)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamDeleteRespSchema.parse(j);
}

export async function apiBrokerTeamRunCreate(
  brokerBase: string,
  teamId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/runs`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return BrokerTeamRunRespSchema.parse(j);
}

export async function apiBrokerTeamRunGet(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamRunStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamRunStatusRespSchema.parse(j);
}

export async function apiBrokerTeamRunCancel(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamRunStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/cancel`, {
    method: "POST",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamRunStatusRespSchema.parse(j);
}

export async function apiBrokerTeamRunApprovalsList(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamRunApprovalListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const team = String(teamId || "").trim();
  const run = String(teamRunId || "").trim();
  if (!team) throw new Error("missing team_id");
  if (!run) throw new Error("missing team_run_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(team)}/runs/${encodeURIComponent(run)}/approvals`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerTeamRunApprovalListRespSchema.parse(j);
}

export async function apiBrokerTeamRunApprovalsCreate(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamRunApprovalListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const team = String(teamId || "").trim();
  const run = String(teamRunId || "").trim();
  if (!team) throw new Error("missing team_id");
  if (!run) throw new Error("missing team_run_id");
  const payload = body ?? {};
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(team)}/runs/${encodeURIComponent(run)}/approvals`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return BrokerTeamRunApprovalListRespSchema.parse(j);
}

export async function apiBrokerTeamRunRuntimeMembersUpdate(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<BrokerTeamRunStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const team = String(teamId || "").trim();
  const run = String(teamRunId || "").trim();
  if (!team) throw new Error("missing team_id");
  if (!run) throw new Error("missing team_run_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(team)}/runs/${encodeURIComponent(run)}/runtime_members`,
    {
      method: "PATCH",
      headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
      body: JSON.stringify(payload),
    },
  );
  const j = await r.json();
  return BrokerTeamRunStatusRespSchema.parse(j);
}
