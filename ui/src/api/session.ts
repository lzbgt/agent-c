import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  AuditSchema,
  type AuditResp,
  DeleteSessionRespSchema,
  type DeleteSessionResp,
  NewSessionRespSchema,
  type NewSessionResp,
  SessionArtifactsSchema,
  SessionAttachmentActionRespSchema,
  type SessionAttachment,
  type SessionAttachmentActionResp,
  type SessionErrorEnvelope,
  type SessionInfo,
  type SessionArtifactsResp,
  SessionClientEventsSchema,
  type SessionClientEventsResp,
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

async function parseJsonSafe(response: Response): Promise<any> {
  const text = await response.text();
  if (!text) return {};
  try {
    return JSON.parse(text);
  } catch {
    return {
      ok: response.ok,
      status: response.status,
      error: text,
    };
  }
}

async function fetchJsonWithFallback(
  urls: string[],
  auth?: ApiAuth,
  init?: RequestInit,
  extraHeaders?: Record<string, string>,
): Promise<any> {
  let last: any = null;
  for (const url of urls) {
    const response = await fetch(url, daemonFetchInit(auth, init, extraHeaders));
    const json = await parseJsonSafe(response);
    const merged = typeof json === "object" && json ? { status: response.status, ...json } : { status: response.status, ok: response.ok };
    last = merged;
    if (response.ok) return merged;
    if (response.status !== 404 && response.status !== 405) return merged;
  }
  return last ?? { ok: false, error: "request failed" };
}

function normalizeSessionId(value: unknown): string | undefined {
  const trimmed = typeof value === "string" ? value.trim() : "";
  return trimmed || undefined;
}

export function extractSessionInfo(payload: unknown): SessionInfo | undefined {
  if (!payload || typeof payload !== "object") return undefined;
  const record = payload as Record<string, any>;
  const nested = record.session;
  if (nested && typeof nested === "object") {
    return {
      ...nested,
      session_id: normalizeSessionId((nested as any).session_id) ?? normalizeSessionId(record.session_id),
      attachment: extractSessionAttachment(nested),
      messages: Array.isArray((nested as any).messages) ? (nested as any).messages : undefined,
    } as SessionInfo;
  }
  const sessionId = normalizeSessionId(record.session_id);
  if (!sessionId && !Array.isArray(record.messages) && !record.attachment) return undefined;
  return {
    ...record,
    session_id: sessionId,
    attachment: extractSessionAttachment(record),
    messages: Array.isArray(record.messages) ? record.messages : undefined,
  } as SessionInfo;
}

export function extractSessionAttachment(payload: unknown): SessionAttachment | undefined {
  if (!payload || typeof payload !== "object") return undefined;
  const record = payload as Record<string, any>;
  const nested = record.attachment;
  if (nested && typeof nested === "object") return nested as SessionAttachment;
  const session = record.session;
  if (session && typeof session === "object" && (session as any).attachment && typeof (session as any).attachment === "object") {
    return (session as any).attachment as SessionAttachment;
  }
  return undefined;
}

export function extractSessionErrorEnvelope(payload: unknown): SessionErrorEnvelope | undefined {
  if (!payload || typeof payload !== "object") return undefined;
  const record = payload as Record<string, any>;
  const nested = record.error;
  if (nested && typeof nested === "object") return nested as SessionErrorEnvelope;
  if (record.error_detail && typeof record.error_detail === "object") return record.error_detail as SessionErrorEnvelope;
  if (record.error_details && typeof record.error_details === "object") {
    return {
      ...(record.error_details as Record<string, any>),
      code: typeof record.code === "string" ? record.code : undefined,
    } as SessionErrorEnvelope;
  }
  if (typeof record.message === "string" || typeof record.code === "string") {
    return {
      code: typeof record.code === "string" ? record.code : undefined,
      message: typeof record.message === "string" ? record.message : undefined,
    };
  }
  return undefined;
}

export function extractSessionErrorMessage(payload: unknown): string | undefined {
  if (!payload || typeof payload !== "object") return undefined;
  const record = payload as Record<string, any>;
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
    const sessionId =
      typeof row === "string"
        ? row.trim()
        : row && typeof row === "object" && typeof (row as any).session_id === "string"
          ? String((row as any).session_id).trim()
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

export async function apiNewSession(
  base: string,
  auth?: ApiAuth,
  opts?: NewSessionOptions,
): Promise<NewSessionResp> {
  const payload: any = {};
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
  const payload: Record<string, any> = {};
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
