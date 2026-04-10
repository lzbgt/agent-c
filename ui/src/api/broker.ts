import { daemonFetchInit, daemonHeaders, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import { safeObject, type UnknownRecord } from "../jsonUtils";
import {
  BrokerAgentsRespSchema,
  BrokerAuthSessionRespSchema,
  BrokerAudioSessionCreateRespSchema,
  BrokerAudioSessionDeleteRespSchema,
  BrokerAudioSessionGetRespSchema,
  BrokerAudioSessionListRespSchema,
  BrokerAudioSignalRespSchema,
  type BrokerAudioSessionCreateResp,
  type BrokerAudioSessionDeleteResp,
  type BrokerAudioSessionGetResp,
  type BrokerAudioSessionListResp,
  type BrokerAudioSignalResp,
  type BrokerAuthSessionResp,
  type BrokerAgentsResp,
  BrokerDeploymentsRespSchema,
  type BrokerDeploymentsResp,
  BrokerMembersRespSchema,
  type BrokerMembersResp,
  BrokerMembershipAuditRespSchema,
  type BrokerMembershipAuditResp,
  BrokerConnectorsRespSchema,
  type BrokerConnectorsResp,
  BrokerEventsReplayRespSchema,
  type BrokerEventsReplayResp,
  BrokerOrchestratorRunRespSchema,
  type BrokerOrchestratorRunCreateRequest,
  type BrokerOrchestratorRunHeartbeatRequest,
  type BrokerOrchestratorRunResp,
  BrokerOrchestratorRunListRespSchema,
  type BrokerOrchestratorRunListResp,
  type BrokerOrchestratorRunUpdateRequest,
  BrokerOrchestratorSpawnRequestRespSchema,
  type BrokerOrchestratorSpawnRequestCreateRequest,
  type BrokerOrchestratorSpawnRequestResp,
  BrokerOrchestratorSpawnRequestListRespSchema,
  type BrokerOrchestratorSpawnRequestListResp,
  type BrokerOrchestratorSpawnRequestUpdateRequest,
  type BrokerTeamCreateRequest,
  BrokerTeamCreateRespSchema,
  type BrokerTeamCreateResp,
  BrokerTeamDeleteRespSchema,
  type BrokerTeamDeleteResp,
  BrokerTeamGetRespSchema,
  type BrokerTeamGetResp,
  BrokerTeamListRespSchema,
  type BrokerTeamListResp,
  type BrokerTeamMemberUpsertRequest,
  type BrokerTeamMemberUpdateRequest,
  BrokerTeamMemberListRespSchema,
  type BrokerTeamMemberListResp,
  BrokerTeamMemberUpsertRespSchema,
  type BrokerTeamMemberUpsertResp,
  type BrokerGuidanceAckRequest,
  BrokerGuidanceAckRespSchema,
  type BrokerGuidanceAckResp,
  type BrokerGuidanceCreateRequest,
  BrokerGuidanceCreateRespSchema,
  type BrokerGuidanceCreateResp,
  BrokerGuidanceListRespSchema,
  type BrokerGuidanceListResp,
  BrokerGuidanceReceiptListRespSchema,
  type BrokerGuidanceReceiptListResp,
  type BrokerTeamQuorumRuleUpsertRequest,
  BrokerTeamQuorumRuleListRespSchema,
  type BrokerTeamQuorumRuleListResp,
  BrokerTeamQuorumRuleUpsertRespSchema,
  type BrokerTeamQuorumRuleUpsertResp,
  type BrokerTeamRunApprovalCreateRequest,
  type BrokerTeamRunGoalUpdateRequest,
  type BrokerTeamRunHandoffUpdateRequest,
  type BrokerTeamRunModeratorDirectiveRequest,
  type BrokerTeamRunModeratorTaskRequest,
  type BrokerTeamRunRequest,
  type BrokerTeamRunRuntimeMembersUpdateRequest,
  BrokerTeamRunRespSchema,
  type BrokerTeamRunResp,
  BrokerTeamRunListRespSchema,
  type BrokerTeamRunListResp,
  BrokerTeamRunStatusRespSchema,
  type BrokerTeamRunStatusResp,
  type BrokerTeamUpdateRequest,
  BrokerTeamRunGoalUpdateRespSchema,
  type BrokerTeamRunGoalUpdateResp,
  BrokerTeamRunHandoffRespSchema,
  type BrokerTeamRunHandoffResp,
  BrokerTeamRunApprovalListRespSchema,
  type BrokerTeamRunApprovalListResp,
  BrokerTeamRunModeratorRespSchema,
  type BrokerTeamRunModeratorResp,
  BrokerTeamRunModeratorEventsRespSchema,
  type BrokerTeamRunModeratorEventsResp,
} from "./schemas/broker";
import {
  ClientPrefsSchema,
  type ClientPrefs,
  ClientPrefsUpdateReqSchema,
  type ClientPrefsUpdateReq,
} from "./schemas/daemon";
import type { MemoryRecapsListParams, MemorySalienceParams } from "./memory";

type BrokerJsonStatusResp = {
  status: number;
  data: UnknownRecord | null;
};

type BrokerAgentMemberMutationResp = {
  ok: boolean;
  error?: string;
};

function withDeploymentIds(body: Record<string, unknown>, deploymentIds?: string[]): Record<string, unknown> {
  const payload: Record<string, unknown> = { ...body };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  return payload;
}

function readBrokerError(value: unknown, fallback = "request failed"): string {
  const record = safeObject(value);
  const message = record.error ?? record.err;
  return typeof message === "string" && message.trim() ? message : fallback;
}

async function parseOptionalJsonRecord(response: Response): Promise<UnknownRecord | null> {
  try {
    return safeObject(await response.json());
  } catch {
    return null;
  }
}

export async function apiBrokerListAgents(brokerBase: string, auth?: ApiAuth): Promise<BrokerAgentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/agents`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerAgentsRespSchema.parse(j);
}

export async function apiBrokerCreateAuthSession(brokerBase: string, auth?: ApiAuth): Promise<BrokerAuthSessionResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/auth/session`, daemonFetchInit(auth, { method: "POST" }));
  const j = await r.json();
  return BrokerAuthSessionRespSchema.parse(j);
}

export async function apiBrokerDeleteAuthSession(brokerBase: string, auth?: ApiAuth): Promise<BrokerAuthSessionResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/auth/session`, daemonFetchInit(auth, { method: "DELETE" }));
  const j = await r.json();
  return BrokerAuthSessionRespSchema.parse(j);
}

export async function apiBrokerListAudioSessions(
  brokerBase: string,
  auth?: ApiAuth,
  opts?: { agentId?: string; deploymentId?: string },
): Promise<BrokerAudioSessionListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const params = new URLSearchParams();
  const agentId = String(opts?.agentId || "").trim();
  const deploymentId = String(opts?.deploymentId || "").trim();
  if (agentId) params.set("agent_id", agentId);
  if (deploymentId) params.set("deployment_id", deploymentId);
  const qs = params.toString();
  const r = await fetch(`${base}/v1/audio/sessions${qs ? `?${qs}` : ""}`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerAudioSessionListRespSchema.parse(j);
}

export async function apiBrokerCreateAudioSession(
  brokerBase: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
): Promise<BrokerAudioSessionCreateResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(
    `${base}/v1/audio/sessions`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body) }, daemonHeaders(auth, { "Content-Type": "application/json" })),
  );
  const j = await r.json();
  return BrokerAudioSessionCreateRespSchema.parse(j);
}

export async function apiBrokerGetAudioSession(
  brokerBase: string,
  sessionId: string,
  auth?: ApiAuth,
): Promise<BrokerAudioSessionGetResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const sid = String(sessionId || "").trim();
  if (!sid) throw new Error("missing session_id");
  const r = await fetch(`${base}/v1/audio/sessions/${encodeURIComponent(sid)}`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerAudioSessionGetRespSchema.parse(j);
}

export async function apiBrokerDeleteAudioSession(
  brokerBase: string,
  sessionId: string,
  auth?: ApiAuth,
): Promise<BrokerAudioSessionDeleteResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const sid = String(sessionId || "").trim();
  if (!sid) throw new Error("missing session_id");
  const r = await fetch(`${base}/v1/audio/sessions/${encodeURIComponent(sid)}`, daemonFetchInit(auth, { method: "DELETE" }));
  const j = await r.json();
  return BrokerAudioSessionDeleteRespSchema.parse(j);
}

export async function apiBrokerSendAudioSignal(
  brokerBase: string,
  sessionId: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
): Promise<BrokerAudioSignalResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const sid = String(sessionId || "").trim();
  if (!sid) throw new Error("missing session_id");
  const r = await fetch(
    `${base}/v1/audio/sessions/${encodeURIComponent(sid)}/signal`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body) }, daemonHeaders(auth, { "Content-Type": "application/json" })),
  );
  const j = await r.json();
  return BrokerAudioSignalRespSchema.parse(j);
}

export async function apiBrokerListConnectors(brokerBase: string, auth?: ApiAuth): Promise<BrokerConnectorsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/connectors`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerConnectorsRespSchema.parse(j);
}

export async function apiBrokerExportConnectors(brokerBase: string, auth?: ApiAuth): Promise<BrokerConnectorsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/connectors/export`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerConnectorsRespSchema.parse(j);
}

export async function apiBrokerListDeployments(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
): Promise<BrokerDeploymentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/deployments`, daemonFetchInit(auth));
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
): Promise<BrokerJsonStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const p = path.startsWith("/") ? path : `/${path}`;
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const dep = typeof deploymentId === "string" ? deploymentId.trim() : "";
  if (dep) headers["X-Agentd-Deployment"] = dep;
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/proxy${p}`,
    daemonFetchInit(auth, { method, body: body === undefined ? undefined : JSON.stringify(body) }, headers),
  );
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerOtaUpdate(
  brokerBase: string,
  agentId: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<BrokerJsonStatusResp> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/update", "POST", body, auth, deploymentId);
}

export async function apiBrokerOtaUpdateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload = withDeploymentIds(body, deploymentIds);
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/ota/update`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, headers),
  );
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerOtaStatus(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<BrokerJsonStatusResp> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/status", "GET", undefined, auth, deploymentId);
}

export async function apiBrokerOtaStatusBulk(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const params = new URLSearchParams();
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    params.set("deployment_ids", deploymentIds.join(","));
  }
  const qs = params.toString();
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/ota/status${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerMemoryRetentionBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload = withDeploymentIds(body ?? {}, deploymentIds);
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/memory/retention/enforce`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, headers),
  );
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsListBulk(
  brokerBase: string,
  agentId: string,
  params: MemoryRecapsListParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
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
  const r = await fetch(url, daemonFetchInit(auth));
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerMemorySalienceBulk(
  brokerBase: string,
  agentId: string,
  params: MemorySalienceParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
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
  const r = await fetch(url, daemonFetchInit(auth));
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsCreateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, unknown>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<BrokerJsonStatusResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload = withDeploymentIds(body ?? {}, deploymentIds);
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/memory/recaps`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, headers),
  );
  const data = await parseOptionalJsonRecord(r);
  return { status: r.status, data };
}

export async function apiBrokerGetMembers(brokerBase: string, agentId: string, auth?: ApiAuth): Promise<BrokerMembersResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerMembersRespSchema.parse(j);
}

export async function apiBrokerUpsertMember(
  brokerBase: string,
  agentId: string,
  req: { user_sub: string; role?: string },
  auth?: ApiAuth,
): Promise<BrokerAgentMemberMutationResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/members`,
    daemonFetchInit(
      auth,
      {
        method: "POST",
        body: JSON.stringify({
          user_sub: String(req?.user_sub || "").trim(),
          role: String(req?.role || "").trim(),
        }),
      },
      { "Content-Type": "application/json" },
    ),
  );
  const j = safeObject(await r.json());
  if (Object.keys(j).length === 0) throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: readBrokerError(j) };
}

export async function apiBrokerDeleteMember(
  brokerBase: string,
  agentId: string,
  userSub: string,
  auth?: ApiAuth,
): Promise<BrokerAgentMemberMutationResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const sub = String(userSub || "").trim();
  if (!sub) throw new Error("missing user_sub");
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/members/${encodeURIComponent(sub)}`,
    daemonFetchInit(auth, { method: "DELETE" }),
  );
  const j = safeObject(await r.json());
  if (Object.keys(j).length === 0) throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: readBrokerError(j) };
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
  const r = await fetch(
    `${base}/v1/agents/${encodeURIComponent(id)}/membership_audit?limit=${encodeURIComponent(String(lim))}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerMembershipAuditRespSchema.parse(j);
}

export async function apiBrokerEventsReplay(
  brokerBase: string,
  auth?: ApiAuth,
  opts?: { sinceTs?: number; limit?: number; types?: string[] },
): Promise<BrokerEventsReplayResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const params = new URLSearchParams();
  if (typeof opts?.sinceTs === "number" && Number.isFinite(opts.sinceTs)) {
    params.set("since_ts", String(Math.max(0, Math.floor(opts.sinceTs))));
  }
  if (typeof opts?.limit === "number" && Number.isFinite(opts.limit)) {
    params.set("limit", String(Math.max(1, Math.floor(opts.limit))));
  }
  if (Array.isArray(opts?.types) && opts?.types.length > 0) {
    params.set(
      "types",
      opts.types
        .map((t) => String(t || "").trim())
        .filter(Boolean)
        .join(","),
    );
  }
  const qs = params.toString();
  const url = `${base}/v1/events/replay${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerEventsReplayRespSchema.parse(j);
}

export async function apiBrokerOrchestratorRunsList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; status?: string },
): Promise<BrokerOrchestratorRunListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const params = new URLSearchParams();
  if (typeof opts?.limit === "number" && Number.isFinite(opts.limit)) {
    params.set("limit", String(Math.max(1, Math.floor(opts.limit))));
  }
  if (typeof opts?.offset === "number" && Number.isFinite(opts.offset)) {
    params.set("offset", String(Math.max(0, Math.floor(opts.offset))));
  }
  if (typeof opts?.status === "string" && opts.status.trim()) {
    params.set("status", opts.status.trim());
  }
  const qs = params.toString();
  const url = `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/runs${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerOrchestratorRunListRespSchema.parse(j);
}

export async function apiBrokerOrchestratorRunCreate(
  brokerBase: string,
  teamId: string,
  body: BrokerOrchestratorRunCreateRequest,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/runs`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerOrchestratorRunRespSchema.parse(j);
}

export async function apiBrokerOrchestratorRunGet(
  brokerBase: string,
  teamId: string,
  runId: string,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(runId || "").trim();
  if (!tid || !rid) throw new Error("missing team_id or run id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/runs/${encodeURIComponent(rid)}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerOrchestratorRunRespSchema.parse(j);
}

export async function apiBrokerOrchestratorRunUpdate(
  brokerBase: string,
  teamId: string,
  runId: string,
  body: BrokerOrchestratorRunUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(runId || "").trim();
  if (!tid || !rid) throw new Error("missing team_id or run id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/runs/${encodeURIComponent(rid)}`,
    daemonFetchInit(auth, { method: "PATCH", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerOrchestratorRunRespSchema.parse(j);
}

export async function apiBrokerOrchestratorRunHeartbeat(
  brokerBase: string,
  teamId: string,
  runId: string,
  body: BrokerOrchestratorRunHeartbeatRequest | undefined,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(runId || "").trim();
  if (!tid || !rid) throw new Error("missing team_id or run id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/runs/${encodeURIComponent(rid)}/heartbeat`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerOrchestratorRunRespSchema.parse(j);
}

export async function apiBrokerOrchestratorSpawnRequestsList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; status?: string; orchestratorRunId?: string },
): Promise<BrokerOrchestratorSpawnRequestListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const params = new URLSearchParams();
  if (typeof opts?.limit === "number" && Number.isFinite(opts.limit)) {
    params.set("limit", String(Math.max(1, Math.floor(opts.limit))));
  }
  if (typeof opts?.offset === "number" && Number.isFinite(opts.offset)) {
    params.set("offset", String(Math.max(0, Math.floor(opts.offset))));
  }
  if (typeof opts?.status === "string" && opts.status.trim()) {
    params.set("status", opts.status.trim());
  }
  if (typeof opts?.orchestratorRunId === "string" && opts.orchestratorRunId.trim()) {
    params.set("orchestrator_run_id", opts.orchestratorRunId.trim());
  }
  const qs = params.toString();
  const url = `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/spawn_requests${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerOrchestratorSpawnRequestListRespSchema.parse(j);
}

export async function apiBrokerOrchestratorSpawnRequestCreate(
  brokerBase: string,
  teamId: string,
  body: BrokerOrchestratorSpawnRequestCreateRequest,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorSpawnRequestResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/spawn_requests`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerOrchestratorSpawnRequestRespSchema.parse(j);
}

export async function apiBrokerOrchestratorSpawnRequestGet(
  brokerBase: string,
  teamId: string,
  spawnRequestId: string,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorSpawnRequestResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const sid = String(spawnRequestId || "").trim();
  if (!tid || !sid) throw new Error("missing team_id or spawn_request_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/spawn_requests/${encodeURIComponent(sid)}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerOrchestratorSpawnRequestRespSchema.parse(j);
}

export async function apiBrokerOrchestratorSpawnRequestUpdate(
  brokerBase: string,
  teamId: string,
  spawnRequestId: string,
  body: BrokerOrchestratorSpawnRequestUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerOrchestratorSpawnRequestResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const sid = String(spawnRequestId || "").trim();
  if (!tid || !sid) throw new Error("missing team_id or spawn_request_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/orchestrator/spawn_requests/${encodeURIComponent(sid)}`,
    daemonFetchInit(auth, { method: "PATCH", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerOrchestratorSpawnRequestRespSchema.parse(j);
}

export async function apiBrokerGetClientPrefs(
  brokerBase: string,
  clientId: string,
  clientKind: string,
  auth?: ApiAuth,
): Promise<ClientPrefs> {
  const base = brokerBase.replace(/\/+$/, "");
  const qs = new URLSearchParams({ client_id: clientId, client_kind: clientKind });
  const r = await fetch(`${base}/v1/client_prefs?${qs.toString()}`, daemonFetchInit(auth));
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
  const r = await fetch(
    `${base}/v1/client_prefs`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return ClientPrefsSchema.parse(j);
}

export async function apiBrokerTeamList(brokerBase: string, auth?: ApiAuth): Promise<BrokerTeamListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/teams`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerTeamListRespSchema.parse(j);
}

export async function apiBrokerTeamCreate(
  brokerBase: string,
  body: BrokerTeamCreateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamCreateResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamCreateRespSchema.parse(j);
}

export async function apiBrokerTeamUpdate(
  brokerBase: string,
  teamId: string,
  body: BrokerTeamUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamGetResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(teamId || "").trim();
  if (!id) throw new Error("missing team_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(id)}`,
    daemonFetchInit(auth, { method: "PATCH", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
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
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}`, daemonFetchInit(auth));
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
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}`, daemonFetchInit(auth, { method: "DELETE" }));
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
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/members`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerTeamMemberListRespSchema.parse(j);
}

export async function apiBrokerTeamMembersUpsert(
  brokerBase: string,
  teamId: string,
  body: BrokerTeamMemberUpsertRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamMemberUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/members`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamMemberUpsertRespSchema.parse(j);
}

export async function apiBrokerTeamMemberUpdate(
  brokerBase: string,
  teamId: string,
  memberId: string,
  body: BrokerTeamMemberUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamMemberUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const mid = String(memberId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!mid) throw new Error("missing member_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/members/${encodeURIComponent(mid)}`,
    daemonFetchInit(auth, { method: "PATCH", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
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
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/members/${encodeURIComponent(mid)}`,
    daemonFetchInit(auth, { method: "DELETE" }),
  );
  const j = await r.json();
  return BrokerTeamDeleteRespSchema.parse(j);
}

export async function apiBrokerTeamGuidanceList(
  brokerBase: string,
  teamId: string,
  params?: {
    teamRunId?: string;
    status?: string;
    sinceTs?: number;
    limit?: number;
    offset?: number;
  },
  auth?: ApiAuth,
): Promise<BrokerGuidanceListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const qs = new URLSearchParams();
  const teamRunId = String(params?.teamRunId || "").trim();
  if (teamRunId) qs.set("team_run_id", teamRunId);
  const status = String(params?.status || "").trim();
  if (status) qs.set("status", status);
  if (Number.isFinite(params?.sinceTs ?? NaN)) qs.set("since_ts", String(params?.sinceTs));
  if (Number.isFinite(params?.limit ?? NaN)) qs.set("limit", String(params?.limit));
  if (Number.isFinite(params?.offset ?? NaN)) qs.set("offset", String(params?.offset));
  const url = `${base}/v1/teams/${encodeURIComponent(tid)}/guidance${qs.toString() ? `?${qs}` : ""}`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerGuidanceListRespSchema.parse(j);
}

export async function apiBrokerTeamGuidanceCreate(
  brokerBase: string,
  teamId: string,
  body: BrokerGuidanceCreateRequest,
  auth?: ApiAuth,
): Promise<BrokerGuidanceCreateResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/guidance`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerGuidanceCreateRespSchema.parse(j);
}

export async function apiBrokerTeamGuidanceAck(
  brokerBase: string,
  teamId: string,
  guidanceId: string,
  body?: BrokerGuidanceAckRequest,
  auth?: ApiAuth,
): Promise<BrokerGuidanceAckResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const gid = String(guidanceId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!gid) throw new Error("missing guidance_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/guidance/${encodeURIComponent(gid)}/ack`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerGuidanceAckRespSchema.parse(j);
}

export async function apiBrokerTeamGuidanceReceiptsList(
  brokerBase: string,
  teamId: string,
  guidanceId: string,
  params?: { limit?: number },
  auth?: ApiAuth,
): Promise<BrokerGuidanceReceiptListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const gid = String(guidanceId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!gid) throw new Error("missing guidance_id");
  const qs = new URLSearchParams();
  if (Number.isFinite(params?.limit ?? NaN)) qs.set("limit", String(params?.limit));
  const url = `${base}/v1/teams/${encodeURIComponent(tid)}/guidance/${encodeURIComponent(gid)}/receipts${
    qs.toString() ? `?${qs}` : ""
  }`;
  const r = await fetch(url, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerGuidanceReceiptListRespSchema.parse(j);
}

export async function apiBrokerTeamQuorumList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
): Promise<BrokerTeamQuorumRuleListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/quorum`, daemonFetchInit(auth));
  const j = await r.json();
  return BrokerTeamQuorumRuleListRespSchema.parse(j);
}

export async function apiBrokerTeamQuorumUpsert(
  brokerBase: string,
  teamId: string,
  body: BrokerTeamQuorumRuleUpsertRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamQuorumRuleUpsertResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/quorum`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
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
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/quorum/${encodeURIComponent(rid)}`,
    daemonFetchInit(auth, { method: "DELETE" }),
  );
  const j = await r.json();
  return BrokerTeamDeleteRespSchema.parse(j);
}

export async function apiBrokerTeamRunCreate(
  brokerBase: string,
  teamId: string,
  body: BrokerTeamRunRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunRespSchema.parse(j);
}

export async function apiBrokerTeamRunList(
  brokerBase: string,
  teamId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; status?: string },
): Promise<BrokerTeamRunListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  if (!tid) throw new Error("missing team_id");
  const q = new URLSearchParams();
  if (typeof opts?.limit === "number") q.set("limit", String(opts.limit));
  if (typeof opts?.offset === "number") q.set("offset", String(opts.offset));
  if (typeof opts?.status === "string" && opts.status.trim()) q.set("status", opts.status.trim());
  const qs = q.toString();
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs${qs ? `?${qs}` : ""}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerTeamRunListRespSchema.parse(j);
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
  const r = await fetch(`${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}`, daemonFetchInit(auth));
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
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/cancel`,
    daemonFetchInit(auth, { method: "POST" }),
  );
  const j = await r.json();
  return BrokerTeamRunStatusRespSchema.parse(j);
}

export async function apiBrokerTeamRunGoalUpdate(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunGoalUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunGoalUpdateResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/goal`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunGoalUpdateRespSchema.parse(j);
}

export async function apiBrokerTeamRunHandoff(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunHandoffUpdateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunHandoffResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/handoff`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunHandoffRespSchema.parse(j);
}

export async function apiBrokerTeamRunModeratorDirective(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunModeratorDirectiveRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunModeratorResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/moderator/directive`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunModeratorRespSchema.parse(j);
}

export async function apiBrokerTeamRunModeratorTask(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunModeratorTaskRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunModeratorResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/moderator/task`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(body ?? {}) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunModeratorRespSchema.parse(j);
}

export async function apiBrokerTeamRunModeratorEvents(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  opts: {
    types?: string;
    maxBytes?: number;
    limit?: number;
    roles?: string;
    memberIds?: string;
    agentIds?: string;
    maxConcurrency?: number;
    timeoutMs?: number;
  },
  auth?: ApiAuth,
): Promise<BrokerTeamRunModeratorEventsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(teamId || "").trim();
  const rid = String(teamRunId || "").trim();
  if (!tid) throw new Error("missing team_id");
  if (!rid) throw new Error("missing team_run_id");
  const q = new URLSearchParams();
  if (opts?.types) q.set("types", opts.types);
  if (typeof opts?.maxBytes === "number") q.set("max_bytes", String(opts.maxBytes));
  if (typeof opts?.limit === "number") q.set("limit", String(opts.limit));
  if (opts?.roles) q.set("roles", opts.roles);
  if (opts?.memberIds) q.set("member_ids", opts.memberIds);
  if (opts?.agentIds) q.set("agent_ids", opts.agentIds);
  if (typeof opts?.maxConcurrency === "number") q.set("max_concurrency", String(opts.maxConcurrency));
  if (typeof opts?.timeoutMs === "number") q.set("timeout_ms", String(opts.timeoutMs));
  const qs = q.toString();
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(tid)}/runs/${encodeURIComponent(rid)}/moderator/events${qs ? `?${qs}` : ""}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerTeamRunModeratorEventsRespSchema.parse(j);
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
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(team)}/runs/${encodeURIComponent(run)}/approvals`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return BrokerTeamRunApprovalListRespSchema.parse(j);
}

export async function apiBrokerTeamRunApprovalsCreate(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunApprovalCreateRequest,
  auth?: ApiAuth,
): Promise<BrokerTeamRunApprovalListResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const team = String(teamId || "").trim();
  const run = String(teamRunId || "").trim();
  if (!team) throw new Error("missing team_id");
  if (!run) throw new Error("missing team_run_id");
  const payload = body ?? {};
  const r = await fetch(
    `${base}/v1/teams/${encodeURIComponent(team)}/runs/${encodeURIComponent(run)}/approvals`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunApprovalListRespSchema.parse(j);
}

export async function apiBrokerTeamRunRuntimeMembersUpdate(
  brokerBase: string,
  teamId: string,
  teamRunId: string,
  body: BrokerTeamRunRuntimeMembersUpdateRequest,
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
    daemonFetchInit(auth, { method: "PATCH", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return BrokerTeamRunStatusRespSchema.parse(j);
}
