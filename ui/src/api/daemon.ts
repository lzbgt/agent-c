import { daemonHeaders, type ApiAuth } from "./auth";
import {
  CapsSchema,
  type Caps,
  DaemonConfigSchema,
  type DaemonConfigResp,
  DaemonConfigUpdateReqSchema,
  type DaemonConfigUpdateReq,
  DaemonConfigUpdateRespSchema,
  type DaemonConfigUpdateResp,
  DiagnosticsProviderTestReqSchema,
  type DiagnosticsProviderTestReq,
  DiagnosticsProviderTestRespSchema,
  type DiagnosticsProviderTestResp,
  DiagnosticsProvidersSchema,
  type DiagnosticsProviders,
  DiagnosticsSchema,
  type Diagnostics,
  ClientPrefsSchema,
  type ClientPrefs,
  ClientPrefsUpdateReqSchema,
  type ClientPrefsUpdateReq,
  HealthSchema,
  type Health,
} from "./schemas/daemon";

async function parseJsonOrThrow(r: Response): Promise<any> {
  const text = await r.text();
  try {
    return JSON.parse(text);
  } catch (err) {
    const msg = text ? `HTTP ${r.status}: ${text}` : `HTTP ${r.status}`;
    throw new Error(msg);
  }
}

export async function apiUpdateDaemonConfig(
  base: string,
  req: DaemonConfigUpdateReq,
  auth?: ApiAuth,
): Promise<DaemonConfigUpdateResp> {
  const payload = DaemonConfigUpdateReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/config/update`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return DaemonConfigUpdateRespSchema.parse(j);
}

export async function apiGetHealth(base: string, auth?: ApiAuth): Promise<Health> {
  const r = await fetch(`${base}/api/v1/health`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return HealthSchema.parse(j);
}

export async function apiGetCaps(base: string, auth?: ApiAuth): Promise<Caps> {
  const r = await fetch(`${base}/api/v1/caps`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return CapsSchema.parse(j);
}

export async function apiGetConfig(base: string, auth?: ApiAuth): Promise<DaemonConfigResp> {
  const r = await fetch(`${base}/api/v1/config`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DaemonConfigSchema.parse(j);
}

export async function apiGetDiagnostics(base: string, auth?: ApiAuth): Promise<Diagnostics> {
  const r = await fetch(`${base}/api/v1/diagnostics`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DiagnosticsSchema.parse(j);
}

export async function apiGetDiagnosticsProviders(base: string, auth?: ApiAuth): Promise<DiagnosticsProviders> {
  const r = await fetch(`${base}/api/v1/diagnostics/providers`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DiagnosticsProvidersSchema.parse(j);
}

export async function apiPostDiagnosticsProviderTest(
  base: string,
  req: DiagnosticsProviderTestReq,
  auth?: ApiAuth,
): Promise<DiagnosticsProviderTestResp> {
  const payload = DiagnosticsProviderTestReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/diagnostics/provider_test`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return DiagnosticsProviderTestRespSchema.parse(j);
}

export async function apiGetClientPrefs(
  base: string,
  clientId: string,
  clientKind: string,
  auth?: ApiAuth,
): Promise<ClientPrefs> {
  const qs = new URLSearchParams({ client_id: clientId, client_kind: clientKind });
  const r = await fetch(`${base}/api/v1/client/prefs?${qs.toString()}`, {
    headers: daemonHeaders(auth),
  });
  const j = await parseJsonOrThrow(r);
  return ClientPrefsSchema.parse(j);
}

export async function apiPostClientPrefs(
  base: string,
  req: ClientPrefsUpdateReq,
  auth?: ApiAuth,
): Promise<ClientPrefs> {
  const payload = ClientPrefsUpdateReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/client/prefs`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await parseJsonOrThrow(r);
  return ClientPrefsSchema.parse(j);
}
