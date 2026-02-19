import { daemonHeaders, type ApiAuth } from "./auth";
import {
  AuditSchema,
  type AuditResp,
  DeleteSessionRespSchema,
  type DeleteSessionResp,
  NewSessionRespSchema,
  type NewSessionResp,
  SessionArtifactsSchema,
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

export async function apiPostSessionUpload(
  base: string,
  req: SessionUploadReq,
  auth?: ApiAuth,
): Promise<SessionUploadResp> {
  const payload = SessionUploadReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/session/upload`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return SessionUploadRespSchema.parse(j);
}

export async function apiListSessions(base: string, auth?: ApiAuth): Promise<SessionsResp> {
  const r = await fetch(`${base}/api/v1/sessions`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return SessionsSchema.parse(j);
}

export async function apiGetSession(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return SessionSchema.parse(j);
}

export async function apiGetSessionScene(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionSceneResp> {
  const r = await fetch(`${base}/api/v1/session/scene?session_id=${encodeURIComponent(sessionId)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return SessionSceneSchema.parse(j);
}

export async function apiPostSessionSceneApply(
  base: string,
  req: SessionSceneApplyReq,
  auth?: ApiAuth,
): Promise<SessionSceneApplyResp> {
  const payload = SessionSceneApplyReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/session/scene/apply`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
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
  const r = await fetch(`${base}/api/v1/session/client_event`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return SessionUiEventRespSchema.parse(j);
}

export async function apiNewSession(
  base: string,
  auth?: ApiAuth,
  opts?: { sessionId?: string; createFiles?: boolean },
): Promise<NewSessionResp> {
  const payload: any = {};
  if (opts?.sessionId) payload.session_id = String(opts.sessionId);
  if (typeof opts?.createFiles === "boolean") payload.create_files = opts.createFiles;
  const r = await fetch(`${base}/api/v1/session/new`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return NewSessionRespSchema.parse(j);
}

export async function apiDeleteSession(base: string, sessionId: string, auth?: ApiAuth): Promise<DeleteSessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return DeleteSessionRespSchema.parse(j);
}

export async function apiGetAudit(base: string, sessionId: string, auth?: ApiAuth): Promise<AuditResp> {
  const r = await fetch(`${base}/api/v1/session/audit?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return AuditSchema.parse(j);
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
    { headers: daemonHeaders(auth) },
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
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return SessionArtifactsSchema.parse(j);
}
