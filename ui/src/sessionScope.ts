export function buildSessionScopeKey(params: {
  profileId?: string;
  base?: string;
  mode?: string;
  deploymentId?: string;
}): string {
  const pid = String(params.profileId || "").trim();
  const base = String(params.base || "").trim();
  const baseKey = base ? `base:${base}` : "base:default";
  const deployment = params.mode === "broker" ? String(params.deploymentId || "").trim() : "";
  const deploymentKey = deployment ? `deployment:${deployment}` : "";
  if (!deploymentKey) return pid ? `profile:${pid}::${baseKey}` : baseKey;
  return pid ? `profile:${pid}::${baseKey}::${deploymentKey}` : `${baseKey}::${deploymentKey}`;
}

export function buildScopedSessionKey(scopeKey: string, sessionId?: string): string {
  const sid = String(sessionId || "").trim() || "default";
  return `${scopeKey}::${sid}`;
}
