import type { BrokerAgentInfo, BrokerTeamRunMemberJobSummary } from "../../api";
import type { RuntimeMemberDraft } from "./teamRunPanelTypes";
import type { TeamMemberRow } from "./types";

export const TEAM_RUN_EVENT_TYPES = new Set([
  "team_run_created",
  "team_run_status",
  "team_runtime_members_updated",
  "team_quorum_request",
  "team_quorum_result",
  "team_goal_progress",
  "team_goal_drift",
  "team_goal_spawn_validation",
  "team_goal_replan_resume",
  "team_handoff",
]);

export const ORCHESTRATOR_EVENT_TYPES = new Set([
  "orchestrator_run_created",
  "orchestrator_run_updated",
  "orchestrator_run_status",
  "orchestrator_run_heartbeat",
  "orchestrator_goal_revision",
  "orchestrator_role_plan_revision",
  "orchestrator_spawn_requested",
  "orchestrator_spawn_updated",
  "orchestrator_spawn_status",
]);

export const GUIDANCE_EVENT_TYPES = new Set([
  "team_guidance_created",
  "team_guidance_ack",
  "team_guidance_expired",
]);

export const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export const fmtSummary = (summary?: BrokerTeamRunMemberJobSummary | null) => {
  if (!summary || typeof summary !== "object") return "";
  const parts: string[] = [];
  const pushIf = (key: keyof BrokerTeamRunMemberJobSummary, label: string) => {
    const val = summary[key];
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

export const normalizeRoleInstructionMap = (raw: unknown): Record<string, string> => {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
  const out: Record<string, string> = {};
  for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
    const role = String(key || "").trim().toLowerCase();
    if (!role) continue;
    let instr = "";
    if (typeof value === "string") instr = value;
    else if (value && typeof value === "object" && !Array.isArray(value)) {
      const instruction = (value as { instruction?: unknown }).instruction;
      if (typeof instruction === "string") instr = instruction;
    }
    instr = instr.trim();
    if (instr) out[role] = instr;
  }
  return out;
};

export const normalizeRolePromptMode = (raw: unknown): string => {
  if (typeof raw !== "string") return "prepend";
  const mode = raw.trim().toLowerCase();
  if (mode === "append" || mode === "replace") return mode;
  return "prepend";
};

export const normalizeSharedMemoryMode = (raw: unknown): string => {
  if (typeof raw !== "string") return "read_write";
  const mode = raw.trim().toLowerCase();
  if (mode === "read_only" || mode === "readonly") return "read_only";
  if (mode === "read_write" || mode === "readwrite") return "read_write";
  return "read_write";
};

export type RoleGraphEdge = {
  from_role: string;
  to_role: string;
  reason?: string;
};

export const normalizeRoleGraphEdges = (raw: unknown): RoleGraphEdge[] => {
  let edgesRaw: unknown[] = [];
  if (Array.isArray(raw)) edgesRaw = raw;
  else if (raw && typeof raw === "object" && !Array.isArray(raw)) {
    const edges = (raw as { edges?: unknown }).edges;
    if (Array.isArray(edges)) edgesRaw = edges;
  }
  const out: RoleGraphEdge[] = [];
  for (const item of edgesRaw) {
    if (!item || typeof item !== "object" || Array.isArray(item)) continue;
    const itemObj = item as Record<string, unknown>;
    const from = String(itemObj.from_role ?? itemObj.from ?? "").trim().toLowerCase();
    const to = String(itemObj.to_role ?? itemObj.to ?? "").trim().toLowerCase();
    if (!from || !to) continue;
    const reason = itemObj.reason ? String(itemObj.reason).trim() : "";
    out.push({ from_role: from, to_role: to, reason: reason || undefined });
  }
  return out;
};

const normalizeRoleValue = (role: unknown): string => String(role || "").trim();

const isConnectedAgent = (agent: BrokerAgentInfo): { connected: boolean; deploymentId: string } => {
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
  runtimeAgentOptions: BrokerAgentInfo[];
  existingRuntime: RuntimeMemberDraft[];
  membersList: TeamMemberRow[];
  role: string;
}): { additions: RuntimeMemberDraft[]; error?: string } => {
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
  const additions: RuntimeMemberDraft[] = [];
  for (const agent of runtimeAgentOptions) {
    const aid = String(agent?.agent_id || "").trim();
    if (!aid || seenAgentIds.has(aid)) continue;
    const { connected, deploymentId } = isConnectedAgent(agent);
    if (!connected) continue;
    const entry: RuntimeMemberDraft = { agent_id: aid, role };
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
  runtimeAgentOptions: BrokerAgentInfo[];
  existingRuntime: RuntimeMemberDraft[];
  membersList: TeamMemberRow[];
  roles: string[];
}): { additions: RuntimeMemberDraft[]; warning?: string; error?: string } => {
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
  const availableAgents: BrokerAgentInfo[] = [];
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
  const additions: RuntimeMemberDraft[] = [];
  let agentIdx = 0;
  for (const role of missingRoles) {
    if (agentIdx >= availableAgents.length) break;
    const agent = availableAgents[agentIdx];
    agentIdx += 1;
    const aid = String(agent?.agent_id || "").trim();
    if (!aid) continue;
    const { deploymentId } = isConnectedAgent(agent);
    const entry: RuntimeMemberDraft = { agent_id: aid, role };
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
