export type ApiAuth =
  | { mode: "direct"; token?: string }
  | { mode: "broker"; token?: string; agentdToken?: string; deploymentId?: string };

function normalizeBearerHeader(raw?: string): string | null {
  const s = typeof raw === "string" ? raw.trim() : "";
  if (!s) return null;
  if (/^bearer\s+/i.test(s)) {
    return `Bearer ${s.replace(/^bearer\s+/i, "").trim()}`;
  }
  return `Bearer ${s}`;
}

// Request headers for talking to:
// - agentd directly: Authorization: Bearer <agentd_token>
// - broker: Authorization: Bearer <oidc_jwt> and optional X-Agentd-Authorization: Bearer <agentd_token>
export function daemonHeaders(auth?: ApiAuth, extra?: Record<string, string>): Record<string, string> {
  const h: Record<string, string> = { ...(extra ?? {}) };
  const authz = normalizeBearerHeader(auth?.token);
  if (authz) h["Authorization"] = authz;
  if (auth && auth.mode === "broker") {
    const agentd = normalizeBearerHeader(auth.agentdToken);
    if (agentd) h["X-Agentd-Authorization"] = agentd;
    const dep = typeof auth.deploymentId === "string" ? auth.deploymentId.trim() : "";
    if (dep) h["X-Agentd-Deployment"] = dep;
  }
  return h;
}
