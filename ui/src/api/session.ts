import { safeJsonParse, safeObject, type UnknownRecord } from "../jsonUtils";
import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  AuditSchema,
  type AuditResp,
  DeleteSessionRespSchema,
  type DeleteSessionResp,
  NewSessionRespSchema,
  type NewSessionResp,
  SessionArtifactsSchema,
  SessionAttachmentSchema,
  SessionAttachmentActionRespSchema,
  SessionOperatorRespSchema,
  type SessionAttachment,
  type SessionAttachmentActionResp,
  type SessionErrorEnvelope,
  type SessionInfo,
  type SessionArtifactsResp,
  type SessionOperatorResp,
  SessionClientEventsSchema,
  type SessionClientEventsResp,
  SessionErrorEnvelopeSchema,
  SessionInfoSchema,
  SessionSchema,
  type SessionResp,
  SessionSceneApplyReqSchema,
  type SessionSceneApplyReq,
  SessionSceneApplyRespSchema,
  type SessionSceneApplyResp,
  SessionSceneSchema,
  type SessionSceneResp,
  SessionUiEventReqSchema,
  type SessionUiEventReq,
  SessionUiEventRespSchema,
  type SessionUiEventResp,
  SessionVoiceControlReqSchema,
  type SessionVoiceControlReq,
  SessionVoiceControlRespSchema,
  type SessionVoiceControlResp,
  SessionVoiceStatsSchema,
  type SessionVoiceStatsResp,
  SessionUploadReqSchema,
  type SessionUploadReq,
  SessionUploadRespSchema,
  type SessionUploadResp,
  SessionsSchema,
  type SessionsResp,
} from "./schemas/session";

type SessionLeaseRequestOptions = {
  clientId?: string;
  leaseSeconds?: number | null;
};

type SessionOperatorMutationOptions = SessionLeaseRequestOptions;

type NewSessionOptions = SessionLeaseRequestOptions & {
  sessionId?: string;
  createFiles?: boolean;
  threadId?: string;
};

function brokerAliasRootFromBase(base: string): string | null {
  const trimmed = String(base || "").trim().replace(/\/+$/, "");
  if (!trimmed) return null;
  const match = trimmed.match(/^(https?:\/\/.+\/v1\/agents\/[^/]+)\/proxy$/i);
  return match ? match[1] : null;
}

function buildSessionAliasUrl(base: string, sessionId: string, suffix?: string): string | null {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = String(sessionId || "").trim();
  if (!aliasRoot || !sid) return null;
  const extra = String(suffix || "").trim().replace(/^\/+/, "");
  return extra ? `${aliasRoot}/sessions/${encodeURIComponent(sid)}/${extra}` : `${aliasRoot}/sessions/${encodeURIComponent(sid)}`;
}

function encodeSessionRef(value: string): string {
  return encodeURIComponent(String(value || "").trim());
}

export function buildSessionEventsStreamUrl(base: string, sessionId: string): string | null {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = String(sessionId || "").trim();
  if (!aliasRoot || !sid) return null;
  return `${aliasRoot}/sessions/${encodeURIComponent(sid)}/events`;
}

function buildCodexwLeaseHeaders(opts?: SessionLeaseRequestOptions): Record<string, string> {
  const headers: Record<string, string> = {};
  const clientId = String(opts?.clientId || "").trim();
  if (clientId) headers["X-Codexw-Client-Id"] = clientId;
  const leaseSeconds =
    typeof opts?.leaseSeconds === "number" && Number.isFinite(opts.leaseSeconds) && opts.leaseSeconds > 0
      ? Math.floor(opts.leaseSeconds)
      : 0;
  if (leaseSeconds > 0) headers["X-Codexw-Lease-Seconds"] = String(leaseSeconds);
  return headers;
}

async function parseJsonSafe(response: Response): Promise<UnknownRecord> {
  const text = await response.text();
  if (!text) return {};
  const parsed = safeJsonParse(text);
  const record = safeObject(parsed);
  if (Object.keys(record).length > 0) return record;
  return {
    ok: response.ok,
    status: response.status,
    error: text,
  };
}

async function fetchJsonWithFallback(
  urls: string[],
  auth?: ApiAuth,
  init?: RequestInit,
  extraHeaders?: Record<string, string>,
): Promise<UnknownRecord> {
  let last: UnknownRecord | null = null;
  for (const url of urls) {
    const response = await fetch(url, daemonFetchInit(auth, init, extraHeaders));
    const json = await parseJsonSafe(response);
    const merged = { status: response.status, ...json };
    last = merged;
    if (response.ok) return merged;
    if (response.status !== 404 && response.status !== 405) return merged;
  }
  return last ?? { ok: false, error: "request failed" };
}

async function fetchSessionOperatorJson(
  base: string,
  sessionId: string,
  rawPaths: string[],
  auth?: ApiAuth,
  init?: RequestInit,
  extraHeaders?: Record<string, string>,
): Promise<SessionOperatorResp> {
  const sid = String(sessionId || "").trim();
  if (!sid) throw new Error("missing session_id");
  const urls: string[] = [];
  for (const rawPath of rawPaths) {
    const clean = String(rawPath || "").trim().replace(/^\/+/, "");
    if (!clean) continue;
    const aliasPath = clean.replace(/^api\/v1\/session\/[^/]+\/?/, "");
    const aliasUrl = buildSessionAliasUrl(base, sid, aliasPath);
    if (aliasUrl) urls.push(aliasUrl);
    urls.push(`${base}/${clean}`);
  }
  const json = await fetchJsonWithFallback(urls, auth, init, extraHeaders);
  return SessionOperatorRespSchema.parse(json);
}

function normalizeSessionId(value: unknown): string | undefined {
  const trimmed = typeof value === "string" ? value.trim() : "";
  return trimmed || undefined;
}

export function extractSessionInfo(payload: unknown): SessionInfo | undefined {
  const record = safeObject(payload);
  const nested = safeObject(record.session);
  if (Object.keys(nested).length > 0) {
    const parsed = SessionInfoSchema.safeParse({
      ...nested,
      session_id: normalizeSessionId(nested.session_id) ?? normalizeSessionId(record.session_id),
      attachment: extractSessionAttachment(nested),
      messages: Array.isArray(nested.messages) ? nested.messages : undefined,
    });
    return parsed.success ? parsed.data : undefined;
  }
  const sessionId = normalizeSessionId(record.session_id);
  if (!sessionId && !Array.isArray(record.messages) && !record.attachment) return undefined;
  const parsed = SessionInfoSchema.safeParse({
    ...record,
    session_id: sessionId,
    attachment: extractSessionAttachment(record),
    messages: Array.isArray(record.messages) ? record.messages : undefined,
  });
  return parsed.success ? parsed.data : undefined;
}

export function extractSessionAttachment(payload: unknown): SessionAttachment | undefined {
  const record = safeObject(payload);
  const nested = SessionAttachmentSchema.safeParse(record.attachment);
  if (nested.success) return nested.data;
  const session = safeObject(record.session);
  const sessionAttachment = SessionAttachmentSchema.safeParse(session.attachment);
  if (sessionAttachment.success) return sessionAttachment.data;
  return undefined;
}

export function extractSessionErrorEnvelope(payload: unknown): SessionErrorEnvelope | undefined {
  const record = safeObject(payload);
  const nested = SessionErrorEnvelopeSchema.safeParse(record.error);
  if (nested.success) return nested.data;
  const errorDetail = SessionErrorEnvelopeSchema.safeParse(record.error_detail);
  if (errorDetail.success) return errorDetail.data;
  const errorDetails = safeObject(record.error_details);
  if (Object.keys(errorDetails).length > 0) {
    const parsed = SessionErrorEnvelopeSchema.safeParse({
      ...errorDetails,
      code: typeof record.code === "string" ? record.code : undefined,
    });
    return parsed.success ? parsed.data : undefined;
  }
  if (typeof record.message === "string" || typeof record.code === "string") {
    const parsed = SessionErrorEnvelopeSchema.safeParse({
      code: typeof record.code === "string" ? record.code : undefined,
      message: typeof record.message === "string" ? record.message : undefined,
    });
    return parsed.success ? parsed.data : undefined;
  }
  return undefined;
}

export function extractSessionErrorMessage(payload: unknown): string | undefined {
  const record = safeObject(payload);
  if (typeof record.error === "string" && record.error.trim()) return record.error.trim();
  if (typeof record.err === "string" && record.err.trim()) return record.err.trim();
  const envelope = extractSessionErrorEnvelope(payload);
  if (typeof envelope?.message === "string" && envelope.message.trim()) return envelope.message.trim();
  if (typeof envelope?.code === "string" && envelope.code.trim()) return envelope.code.trim();
  return undefined;
}

export function extractSessionIds(payload: SessionsResp | undefined): string[] {
  const raw = Array.isArray(payload?.sessions) ? payload.sessions : [];
  const out: string[] = [];
  const seen = new Set<string>();
  for (const row of raw) {
    const record = safeObject(row);
    const sessionId =
      typeof row === "string"
        ? row.trim()
        : typeof record.session_id === "string"
          ? record.session_id.trim()
          : "";
    if (!sessionId || seen.has(sessionId)) continue;
    seen.add(sessionId);
    out.push(sessionId);
  }
  return out;
}

export async function apiPostSessionUpload(
  base: string,
  req: SessionUploadReq,
  auth?: ApiAuth,
): Promise<SessionUploadResp> {
  const payload = SessionUploadReqSchema.parse(req);
  const r = await fetch(
    `${base}/api/v1/session/upload`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return SessionUploadRespSchema.parse(j);
}

export async function apiListSessions(base: string, auth?: ApiAuth): Promise<SessionsResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const json = await fetchJsonWithFallback(
    aliasRoot ? [`${aliasRoot}/sessions`, `${base}/api/v1/sessions`, `${base}/api/v1/session`] : [`${base}/api/v1/sessions`, `${base}/api/v1/session`],
    auth,
  );
  return SessionsSchema.parse(json);
}

export async function apiGetSession(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    aliasRoot
      ? [`${aliasRoot}/sessions/${sid}`, `${base}/api/v1/session?session_id=${sid}`, `${base}/api/v1/session/${sid}`]
      : [`${base}/api/v1/session?session_id=${sid}`, `${base}/api/v1/session/${sid}`],
    auth,
  );
  return SessionSchema.parse(json);
}

export async function apiGetSessionScene(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionSceneResp> {
  const r = await fetch(`${base}/api/v1/session/scene?session_id=${encodeURIComponent(sessionId)}`, daemonFetchInit(auth));
  const j = await r.json();
  return SessionSceneSchema.parse(j);
}

export async function apiPostSessionSceneApply(
  base: string,
  req: SessionSceneApplyReq,
  auth?: ApiAuth,
): Promise<SessionSceneApplyResp> {
  const payload = SessionSceneApplyReqSchema.parse(req);
  const r = await fetch(
    `${base}/api/v1/session/scene/apply`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return SessionSceneApplyRespSchema.parse(j);
}

export async function apiPostSessionUiEvent(
  base: string,
  req: SessionUiEventReq,
  auth?: ApiAuth,
): Promise<SessionUiEventResp> {
  const payload = SessionUiEventReqSchema.parse(req);
  // Preferred endpoint name is /session/client_event; /session/ui_event remains as a legacy alias.
  const r = await fetch(
    `${base}/api/v1/session/client_event`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return SessionUiEventRespSchema.parse(j);
}

export async function apiPostSessionVoiceControl(
  base: string,
  req: SessionVoiceControlReq,
  auth?: ApiAuth,
): Promise<SessionVoiceControlResp> {
  const payload = SessionVoiceControlReqSchema.parse(req);
  const r = await fetch(
    `${base}/api/v1/session/voice_control`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return SessionVoiceControlRespSchema.parse(j);
}

export async function apiGetSessionVoiceStats(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number },
): Promise<SessionVoiceStatsResp> {
  const maxBytes = typeof opts?.maxBytes === "number" ? opts.maxBytes : 1024 * 1024;
  const r = await fetch(
    `${base}/api/v1/session/voice_stats?session_id=${encodeURIComponent(sessionId)}&max_bytes=${encodeURIComponent(String(maxBytes))}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return SessionVoiceStatsSchema.parse(j);
}

export async function apiNewSession(
  base: string,
  auth?: ApiAuth,
  opts?: NewSessionOptions,
): Promise<NewSessionResp> {
  const payload: Record<string, unknown> = {};
  if (opts?.sessionId) payload.session_id = String(opts.sessionId);
  if (typeof opts?.createFiles === "boolean") payload.create_files = opts.createFiles;
  if (opts?.threadId) payload.thread_id = String(opts.threadId).trim();
  const aliasRoot = brokerAliasRootFromBase(base);
  const json = await fetchJsonWithFallback(
    aliasRoot ? [`${aliasRoot}/sessions`, `${base}/api/v1/session/new`] : [`${base}/api/v1/session/new`],
    auth,
    { method: "POST", body: JSON.stringify(payload) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
  return NewSessionRespSchema.parse(json);
}

export async function apiDeleteSession(base: string, sessionId: string, auth?: ApiAuth): Promise<DeleteSessionResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    aliasRoot ? [`${aliasRoot}/sessions/${sid}`, `${base}/api/v1/session?session_id=${sid}`] : [`${base}/api/v1/session?session_id=${sid}`],
    auth,
    { method: "DELETE" },
  );
  return DeleteSessionRespSchema.parse(json);
}

export async function apiGetAudit(base: string, sessionId: string, auth?: ApiAuth): Promise<AuditResp> {
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    [`${base}/api/v1/session/audit?session_id=${sid}&max_bytes=1048576`],
    auth,
  );
  return AuditSchema.parse(json);
}

export async function apiGetSessionClientEvents(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number },
): Promise<SessionClientEventsResp> {
  const maxBytes = typeof opts?.maxBytes === "number" ? opts.maxBytes : 1024 * 1024;
  const r = await fetch(
    `${base}/api/v1/session/client_events?session_id=${encodeURIComponent(sessionId)}&max_bytes=${encodeURIComponent(String(maxBytes))}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return SessionClientEventsSchema.parse(j);
}

export async function apiGetSessionArtifacts(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number; maxArtifacts?: number },
): Promise<SessionArtifactsResp> {
  const maxBytes = opts?.maxBytes ?? 2 * 1024 * 1024;
  const maxArtifacts = opts?.maxArtifacts ?? 64;
  const r = await fetch(
    `${base}/api/v1/session/artifacts?session_id=${encodeURIComponent(sessionId)}&max_bytes=${encodeURIComponent(
      String(maxBytes),
    )}&max_artifacts=${encodeURIComponent(String(maxArtifacts))}`,
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return SessionArtifactsSchema.parse(j);
}

export async function apiAttachSession(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: SessionLeaseRequestOptions & { threadId?: string },
): Promise<SessionAttachmentActionResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const payload: Record<string, unknown> = {};
  if (opts?.threadId) payload.thread_id = String(opts.threadId).trim();
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    aliasRoot ? [`${aliasRoot}/sessions/${sid}/attach`, `${base}/api/v1/session/attach`] : [`${base}/api/v1/session/attach`],
    auth,
    {
      method: "POST",
      body: JSON.stringify(payload),
    },
    {
      "Content-Type": "application/json",
      ...buildCodexwLeaseHeaders(opts),
    },
  );
  return SessionAttachmentActionRespSchema.parse(json);
}

export async function apiRenewSessionAttachment(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: SessionLeaseRequestOptions,
): Promise<SessionAttachmentActionResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    aliasRoot
      ? [`${aliasRoot}/sessions/${sid}/attachment/renew`, `${base}/api/v1/session/${sid}/attachment/renew`]
      : [`${base}/api/v1/session/${sid}/attachment/renew`],
    auth,
    {
      method: "POST",
      body: JSON.stringify({
        client_id: normalizeSessionId(opts?.clientId),
        lease_seconds:
          typeof opts?.leaseSeconds === "number" && Number.isFinite(opts.leaseSeconds) && opts.leaseSeconds > 0
            ? Math.floor(opts.leaseSeconds)
            : undefined,
      }),
    },
    {
      "Content-Type": "application/json",
      ...buildCodexwLeaseHeaders(opts),
    },
  );
  return SessionAttachmentActionRespSchema.parse(json);
}

export async function apiReleaseSessionAttachment(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: SessionLeaseRequestOptions,
): Promise<SessionAttachmentActionResp> {
  const aliasRoot = brokerAliasRootFromBase(base);
  const sid = encodeURIComponent(sessionId);
  const json = await fetchJsonWithFallback(
    aliasRoot
      ? [`${aliasRoot}/sessions/${sid}/attachment/release`, `${base}/api/v1/session/${sid}/attachment/release`]
      : [`${base}/api/v1/session/${sid}/attachment/release`],
    auth,
    {
      method: "POST",
      body: JSON.stringify({
        client_id: normalizeSessionId(opts?.clientId),
      }),
    },
    {
      "Content-Type": "application/json",
      ...buildCodexwLeaseHeaders(opts),
    },
  );
  return SessionAttachmentActionRespSchema.parse(json);
}

export async function apiGetSessionOrchestrationStatus(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/orchestration/status`], auth);
}

export async function apiGetSessionOrchestrationWorkers(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/orchestration/workers`], auth);
}

export async function apiGetSessionOrchestrationDependencies(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/orchestration/dependencies`], auth);
}

export async function apiListSessionShells(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/shells`], auth);
}

export async function apiStartSessionShell(
  base: string,
  sessionId: string,
  body: { command: string; intent?: string; label?: string },
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  const sid = encodeSessionRef(sessionId);
  const aliasUrl = buildSessionAliasUrl(base, sessionId, "shells");
  const urls = aliasUrl ? [aliasUrl, `${base}/api/v1/session/${sid}/shells/start`] : [`${base}/api/v1/session/${sid}/shells/start`];
  const json = await fetchJsonWithFallback(
    urls,
    auth,
    { method: "POST", body: JSON.stringify(body ?? {}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
  return SessionOperatorRespSchema.parse(json);
}

export async function apiGetSessionShell(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/shells/${encodeSessionRef(jobRef)}`],
    auth,
  );
}

export async function apiPollSessionShell(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/shells/${encodeSessionRef(jobRef)}/poll`],
    auth,
    { method: "POST", body: JSON.stringify({}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiSendSessionShell(
  base: string,
  sessionId: string,
  jobRef: string,
  text: string,
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/shells/${encodeSessionRef(jobRef)}/send`],
    auth,
    { method: "POST", body: JSON.stringify({ text }) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiTerminateSessionShell(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/shells/${encodeSessionRef(jobRef)}/terminate`],
    auth,
    { method: "POST", body: JSON.stringify({}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiListSessionServices(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/services`], auth);
}

export async function apiGetSessionService(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/services/${encodeSessionRef(jobRef)}`],
    auth,
  );
}

export async function apiAttachSessionService(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/services/${encodeSessionRef(jobRef)}/attach`],
    auth,
    { method: "POST", body: JSON.stringify({}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiWaitSessionService(
  base: string,
  sessionId: string,
  jobRef: string,
  auth?: ApiAuth,
  body?: { timeout_ms?: number },
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/services/${encodeSessionRef(jobRef)}/wait`],
    auth,
    { method: "POST", body: JSON.stringify(body ?? {}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiRunSessionService(
  base: string,
  sessionId: string,
  jobRef: string,
  body: { recipe: string; args?: Record<string, unknown> },
  auth?: ApiAuth,
  opts?: SessionOperatorMutationOptions,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/services/${encodeSessionRef(jobRef)}/run`],
    auth,
    { method: "POST", body: JSON.stringify(body ?? {}) },
    { "Content-Type": "application/json", ...buildCodexwLeaseHeaders(opts) },
  );
}

export async function apiListSessionCapabilities(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(base, sessionId, [`api/v1/session/${encodeSessionRef(sessionId)}/capabilities`], auth);
}

export async function apiGetSessionCapability(
  base: string,
  sessionId: string,
  capability: string,
  auth?: ApiAuth,
): Promise<SessionOperatorResp> {
  return fetchSessionOperatorJson(
    base,
    sessionId,
    [`api/v1/session/${encodeSessionRef(sessionId)}/capabilities/${encodeSessionRef(capability)}`],
    auth,
  );
}
