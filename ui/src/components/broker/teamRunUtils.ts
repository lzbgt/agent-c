export const TEAM_RUN_EVENT_TYPES = new Set([
  "team_run_created",
  "team_run_status",
  "team_runtime_members_updated",
  "team_quorum_request",
  "team_quorum_result",
  "team_goal_progress",
  "team_goal_drift",
]);

export const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export const fmtSummary = (summary?: any) => {
  if (!summary || typeof summary !== "object") return "";
  const parts: string[] = [];
  const pushIf = (key: string, label: string) => {
    const val = summary?.[key];
    if (typeof val === "number") parts.push(`${label} ${val}`);
  };
  pushIf("total", "total");
  pushIf("queued", "queued");
  pushIf("running", "running");
  pushIf("done", "done");
  pushIf("error", "error");
  pushIf("cancelled", "cancelled");
  pushIf("interrupted", "interrupted");
  pushIf("unknown", "unknown");
  pushIf("ok", "ok");
  pushIf("failed", "failed");
  pushIf("dispatch_errors", "dispatch_errors");
  return parts.join(" · ");
};

export const parseCsvList = (raw?: string | null) => {
  if (!raw) return [];
  return String(raw)
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);
};

export const normalizeRoleInstructionMap = (raw: any): Record<string, string> => {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
  const out: Record<string, string> = {};
  for (const [key, value] of Object.entries(raw)) {
    const role = String(key || "").trim().toLowerCase();
    if (!role) continue;
    let instr = "";
    if (typeof value === "string") instr = value;
    else if (value && typeof value === "object" && typeof (value as any).instruction === "string") {
      instr = String((value as any).instruction);
    }
    instr = instr.trim();
    if (instr) out[role] = instr;
  }
  return out;
};

export const normalizeRolePromptMode = (raw: any): string => {
  if (typeof raw !== "string") return "prepend";
  const mode = raw.trim().toLowerCase();
  if (mode === "append" || mode === "replace") return mode;
  return "prepend";
};

const normalizeRoleValue = (role: any): string => String(role || "").trim();

const isConnectedAgent = (agent: any): { connected: boolean; deploymentId: string } => {
  const deployments = Array.isArray(agent?.deployments) ? agent.deployments : [];
  const connected = agent?.connected === true || deployments.length > 0;
  let deploymentId = "";
  if (deployments.length > 0 && deployments[0]?.deployment_id) {
    deploymentId = String(deployments[0].deployment_id);
  }
  return { connected, deploymentId };
};

export const buildRuntimeAgentAdditions = ({
  runtimeAgentOptions,
  existingRuntime,
  membersList,
  role,
}: {
  runtimeAgentOptions: any[];
  existingRuntime: any[];
  membersList: any[];
  role: string;
}): { additions: any[]; error?: string } => {
  if (runtimeAgentOptions.length === 0) {
    return { additions: [], error: "load agents before adding connected agents" };
  }
  const seenAgentIds = new Set<string>();
  for (const m of membersList) {
    const aid = String(m?.agent_id || "").trim();
    if (aid) seenAgentIds.add(aid);
  }
  for (const item of existingRuntime) {
    const aid = item?.agent_id ? String(item.agent_id).trim() : "";
    if (aid) seenAgentIds.add(aid);
  }
  const additions: any[] = [];
  for (const agent of runtimeAgentOptions) {
    const aid = String(agent?.agent_id || "").trim();
    if (!aid || seenAgentIds.has(aid)) continue;
    const { connected, deploymentId } = isConnectedAgent(agent);
    if (!connected) continue;
    const entry: Record<string, any> = { agent_id: aid, role };
    if (deploymentId) entry.deployment_id = deploymentId;
    additions.push(entry);
    seenAgentIds.add(aid);
  }
  if (additions.length === 0) {
    return { additions: [], error: "no connected agents to add (already in team/runtime)" };
  }
  return { additions };
};

export const buildRoleAllocatedRuntimeMembers = ({
  runtimeAgentOptions,
  existingRuntime,
  membersList,
  roles,
}: {
  runtimeAgentOptions: any[];
  existingRuntime: any[];
  membersList: any[];
  roles: string[];
}): { additions: any[]; warning?: string; error?: string } => {
  if (runtimeAgentOptions.length === 0) {
    return { additions: [], error: "load agents before allocating by role" };
  }
  const roleOrder: string[] = [];
  const roleMap = new Map<string, string>();
  for (const role of roles) {
    const raw = normalizeRoleValue(role);
    if (!raw) continue;
    const key = raw.toLowerCase();
    if (roleMap.has(key)) continue;
    roleMap.set(key, raw);
    roleOrder.push(raw);
  }
  if (roleOrder.length === 0) {
    return { additions: [], error: "no role plan roles available" };
  }
  const existingRoles = new Set<string>();
  const seenAgentIds = new Set<string>();
  for (const m of membersList) {
    const aid = String(m?.agent_id || "").trim();
    if (aid) seenAgentIds.add(aid);
    const role = normalizeRoleValue(m?.role).toLowerCase();
    if (role) existingRoles.add(role);
  }
  for (const item of existingRuntime) {
    const aid = item?.agent_id ? String(item.agent_id).trim() : "";
    if (aid) seenAgentIds.add(aid);
    const role = normalizeRoleValue(item?.role).toLowerCase();
    if (role) existingRoles.add(role);
  }
  const missingRoles = roleOrder.filter((role) => !existingRoles.has(role.toLowerCase()));
  if (missingRoles.length === 0) {
    return { additions: [], error: "all role plan roles are already assigned" };
  }
  const availableAgents: any[] = [];
  for (const agent of runtimeAgentOptions) {
    const aid = String(agent?.agent_id || "").trim();
    if (!aid || seenAgentIds.has(aid)) continue;
    const { connected } = isConnectedAgent(agent);
    if (!connected) continue;
    availableAgents.push(agent);
  }
  if (availableAgents.length === 0) {
    return { additions: [], error: "no connected agents available for role allocation" };
  }
  const additions: any[] = [];
  let agentIdx = 0;
  for (const role of missingRoles) {
    if (agentIdx >= availableAgents.length) break;
    const agent = availableAgents[agentIdx];
    agentIdx += 1;
    const aid = String(agent?.agent_id || "").trim();
    if (!aid) continue;
    const { deploymentId } = isConnectedAgent(agent);
    const entry: Record<string, any> = { agent_id: aid, role };
    if (deploymentId) entry.deployment_id = deploymentId;
    additions.push(entry);
  }
  if (additions.length === 0) {
    return { additions: [], error: "no connected agents available for role allocation" };
  }
  const warning =
    additions.length < missingRoles.length
      ? `allocated ${additions.length}/${missingRoles.length} roles (insufficient connected agents)`
      : undefined;
  return { additions, warning };
};
