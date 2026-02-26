import React from "react";
import {
  apiBrokerTeamCreate,
  apiBrokerTeamDelete,
  apiBrokerTeamGet,
  apiBrokerTeamList,
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiBrokerTeamMembersDelete,
  apiBrokerTeamMembersList,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamMemberUpdate,
  apiBrokerListAgents,
  apiBrokerEventsReplay,
  apiBrokerTeamQuorumDelete,
  apiBrokerTeamQuorumList,
  apiBrokerTeamQuorumUpsert,
  apiBrokerTeamUpdate,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import FieldLabel from "../FieldLabel";
import BrokerTeamRunPanel from "./BrokerTeamRunPanel";
import BrokerOrchestratorRunPanel from "./BrokerOrchestratorRunPanel";
import BrokerOrchestratorSpawnPanel from "./BrokerOrchestratorSpawnPanel";
import BrokerTeamGuidancePanel from "./BrokerTeamGuidancePanel";
import BrokerTeamCreatePanel from "./BrokerTeamCreatePanel";
import BrokerTeamSettingsPanel from "./BrokerTeamSettingsPanel";
import {
  fmtTs,
  normalizeRoleGraphEdges,
  normalizeRoleInstructionMap,
  normalizeRolePromptMode,
  normalizeSharedMemoryMode,
  type RoleGraphEdge,
} from "./teamRunUtils";
import { GUIDANCE_EVENT_TYPES, ORCHESTRATOR_EVENT_TYPES, TEAM_RUN_EVENT_TYPES } from "./teamRunUtils";
import type { BrokerEventRow, TeamCursorEntry, TeamMemberRow, TeamQuorumRuleRow } from "./types";

const TEAM_EVENTS_MAX = 200;
const TEAM_EVENTS_PREFS_KIND = "webui-team-events";
const TEAM_EVENTS_PREFS_VERSION = 1;

type SectionCardProps = {
  title: string;
  description?: string;
  defaultOpen?: boolean;
  children: React.ReactNode;
};

function SectionCard({ title, description, defaultOpen = false, children }: SectionCardProps) {
  const [open, setOpen] = React.useState<boolean>(defaultOpen);
  return (
    <details
      className="rounded-md border border-white/10 bg-black/20 p-3"
      open={open}
      onToggle={(event) => setOpen((event.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs font-semibold text-white/80">
        <div className="flex items-center justify-between gap-2">
          <span>{title}</span>
          <span className="text-[11px] text-white/40">Toggle</span>
        </div>
        {description ? <div className="text-[11px] font-normal text-white/50">{description}</div> : null}
      </summary>
      <div className="mt-3 grid gap-3">{children}</div>
    </details>
  );
}

export type BrokerTeamConsoleProps = {
  base: string;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
  quorumEvents?: BrokerEventRow[];
};

export default function BrokerTeamConsole(props: BrokerTeamConsoleProps) {
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const canQuery = props.base.length > 0 && authToken.length > 0;

  const [teamsBusy, setTeamsBusy] = React.useState<boolean>(false);
  const [teamsError, setTeamsError] = React.useState<string | null>(null);
  type TeamRow = {
    team_id?: string;
    display_name?: string;
    owner_sub?: string;
    created_unix_ms?: number;
    tags?: string[];
    policy_ref?: string;
    shared_memory_scope_id?: string;
    meta?: Record<string, any>;
  };

  const [teams, setTeams] = React.useState<TeamRow[] | null>(null);
  const [teamId, setTeamId] = React.useState<string>("");
  const [newTeamId, setNewTeamId] = React.useState<string>("");
  const [newTeamName, setNewTeamName] = React.useState<string>("");
  const [teamDetails, setTeamDetails] = React.useState<any | null>(null);
  const [teamEditName, setTeamEditName] = React.useState<string>("");
  const [teamEditTags, setTeamEditTags] = React.useState<string>("");
  const [teamEditPolicyRef, setTeamEditPolicyRef] = React.useState<string>("");
  const [teamEditSharedScope, setTeamEditSharedScope] = React.useState<string>("");
  const [teamEditSharedMode, setTeamEditSharedMode] = React.useState<string>("read_write");
  const [teamEditMetaJson, setTeamEditMetaJson] = React.useState<string>("");
  const [teamEditRoleOverridesJson, setTeamEditRoleOverridesJson] = React.useState<string>("");
  const [teamRoleInstructions, setTeamRoleInstructions] = React.useState<Record<string, string>>({});
  const [teamRolePromptMode, setTeamRolePromptMode] = React.useState<string>("prepend");
  const [teamRoleGraphEdges, setTeamRoleGraphEdges] = React.useState<RoleGraphEdge[]>([]);
  const [teamRolePlanTouched, setTeamRolePlanTouched] = React.useState<boolean>(false);
  const [teamEditBusy, setTeamEditBusy] = React.useState<boolean>(false);
  const [teamEditError, setTeamEditError] = React.useState<string | null>(null);
  const teamTabs = React.useMemo(
    () => [
      { id: "run", label: "Run" },
      { id: "members", label: "Members" },
      { id: "setup", label: "Setup" },
      { id: "settings", label: "Settings" },
      { id: "advanced", label: "Advanced" },
    ],
    [],
  );
  const [teamTab, setTeamTab] = useLocalStorageState<string>("agentui.teamTab", "run");
  const teamTabIds = React.useMemo(() => new Set(teamTabs.map((t) => t.id)), [teamTabs]);
  React.useEffect(() => {
    if (!teamTabIds.has(teamTab)) setTeamTab("run");
  }, [teamTab, teamTabIds, setTeamTab]);

  const [teamReplayEvents, setTeamReplayEvents] = React.useState<BrokerEventRow[]>([]);
  const [orchestratorReplayEvents, setOrchestratorReplayEvents] = React.useState<BrokerEventRow[]>([]);
  const [teamReplayBusy, setTeamReplayBusy] = React.useState<boolean>(false);
  const [teamReplayError, setTeamReplayError] = React.useState<string | null>(null);
  const [teamReplayNote, setTeamReplayNote] = React.useState<string | null>(null);
  const [teamEventsCursorByTeam, setTeamEventsCursorByTeam] = useLocalStorageState<Record<string, number>>(
    "agentui.teamEventsCursorByTeam",
    {},
  );
  const teamEventsCursorRef = React.useRef<number>(0);
  const [teamEventsCursorByScope, setTeamEventsCursorByScope] = React.useState<Record<string, TeamCursorEntry>>({});
  const [teamEventsCursorStatus, setTeamEventsCursorStatus] = React.useState<"idle" | "loading" | "ready" | "error">(
    "idle",
  );
  const teamEventsCursorPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: Record<string, TeamCursorEntry> | null;
  }>({ timer: null, pending: null });

  const [membersBusy, setMembersBusy] = React.useState<boolean>(false);
  const [membersError, setMembersError] = React.useState<string | null>(null);

  const [quickTeamName, setQuickTeamName] = React.useState<string>("");
  const [quickTeamId, setQuickTeamId] = React.useState<string>("");
  const [quickTeamGoal, setQuickTeamGoal] = React.useState<string>("");
  const [quickBuilderError, setQuickBuilderError] = React.useState<string | null>(null);
  const [quickBuilderBusy, setQuickBuilderBusy] = React.useState<boolean>(false);
  const [quickTemplate, setQuickTemplate] = React.useState<string>("standard");
  type QuickMember = {
    id: string;
    role: string;
    provider: string;
    model: string;
    baseUrl: string;
    agentId: string;
    deploymentId: string;
  };
  const providerDefaults: Record<string, string> = {
    openai: "https://api.openai.com/v1",
    anthropic: "https://api.anthropic.com",
    deepseek: "https://api.deepseek.com",
    kimi: "https://api.moonshot.cn/v1",
    glm: "https://open.bigmodel.cn/api/paas/v4",
    local: "",
    custom: "",
  };
  const providerModelDefaults: Record<string, string> = {
    openai: "gpt-4.1",
    anthropic: "claude-3-7-sonnet-20250219",
    deepseek: "deepseek-chat",
    kimi: "kimi-k2.5",
    glm: "glm-4",
    local: "",
    custom: "",
  };
  const makeQuickMembers = React.useCallback((template: string): QuickMember[] => {
    const mk = (role: string): QuickMember => ({
      id: `${role}-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
      role,
      provider: "openai",
      model: providerModelDefaults.openai,
      baseUrl: providerDefaults.openai,
      agentId: "",
      deploymentId: "",
    });
    if (template === "planner_executor") return [mk("planner"), mk("executor")];
    if (template === "research_team") return [mk("researcher"), mk("executor"), mk("critic")];
    return [mk("planner"), mk("executor"), mk("critic")];
  }, []);
  const [quickMembers, setQuickMembers] = React.useState<QuickMember[]>(() => makeQuickMembers("standard"));

  const [members, setMembers] = React.useState<TeamMemberRow[] | null>(null);
  const [memberId, setMemberId] = React.useState<string>("");
  const [memberRole, setMemberRole] = React.useState<string>("executor");
  const [memberStatus, setMemberStatus] = React.useState<string>("active");
  const [memberWeight, setMemberWeight] = React.useState<string>("1");
  const [memberCapabilities, setMemberCapabilities] = React.useState<string>("");
  const [memberAgentId, setMemberAgentId] = React.useState<string>("");
  const [memberDeploymentId, setMemberDeploymentId] = React.useState<string>("");
  const [memberBackendLabel, setMemberBackendLabel] = React.useState<string>("");
  const [memberModel, setMemberModel] = React.useState<string>("");
  const [memberBaseUrl, setMemberBaseUrl] = React.useState<string>("");
  const [memberSummaryModel, setMemberSummaryModel] = React.useState<string>("");
  const [memberTools, setMemberTools] = React.useState<string>("");
  const [memberTimeoutMs, setMemberTimeoutMs] = React.useState<string>("");
  const [memberEditId, setMemberEditId] = React.useState<string>("");
  const [memberEditRole, setMemberEditRole] = React.useState<string>("");
  const [memberEditStatus, setMemberEditStatus] = React.useState<string>("");
  const [memberEditWeight, setMemberEditWeight] = React.useState<string>("");
  const [memberEditCapabilities, setMemberEditCapabilities] = React.useState<string>("");
  const [memberEditMetaJson, setMemberEditMetaJson] = React.useState<string>("");
  const [memberEditBackendLabel, setMemberEditBackendLabel] = React.useState<string>("");
  const [memberEditModel, setMemberEditModel] = React.useState<string>("");
  const [memberEditBaseUrl, setMemberEditBaseUrl] = React.useState<string>("");
  const [memberEditSummaryModel, setMemberEditSummaryModel] = React.useState<string>("");
  const [memberEditTools, setMemberEditTools] = React.useState<string>("");
  const [memberEditTimeoutMs, setMemberEditTimeoutMs] = React.useState<string>("");
  const [memberEditAgentId, setMemberEditAgentId] = React.useState<string>("");
  const [memberEditDeploymentId, setMemberEditDeploymentId] = React.useState<string>("");
  const [memberEditBusy, setMemberEditBusy] = React.useState<boolean>(false);
  const [memberEditError, setMemberEditError] = React.useState<string | null>(null);
  const [memberAgentsBusy, setMemberAgentsBusy] = React.useState<boolean>(false);
  const [memberAgentsError, setMemberAgentsError] = React.useState<string | null>(null);
  const [memberAgents, setMemberAgents] = React.useState<any[] | null>(null);

  const [rulesBusy, setRulesBusy] = React.useState<boolean>(false);
  const [rulesError, setRulesError] = React.useState<string | null>(null);
  const [rules, setRules] = React.useState<TeamQuorumRuleRow[] | null>(null);
  const [ruleAction, setRuleAction] = React.useState<string>("team_run");
  const [ruleMinApprovals, setRuleMinApprovals] = React.useState<string>("1");
  const [ruleMode, setRuleMode] = React.useState<string>("strict");

  const teamList = Array.isArray(teams) ? teams : [];
  const teamIdTrimmed = String(teamId || "").trim();
  React.useEffect(() => {
    if (teamList.length === 0 && teamTab !== "setup") {
      setTeamTab("setup");
    }
  }, [teamList.length, teamTab, setTeamTab]);
  const teamEventsCursor = teamIdTrimmed ? teamEventsCursorByTeam[teamIdTrimmed] || 0 : 0;
  const teamEventsScopeKey = React.useMemo(() => {
    if (!teamIdTrimmed) return "";
    const base = String(props.base || "").trim();
    const key = String(props.authKey || "").trim();
    return `${base}::${key}::${teamIdTrimmed}`;
  }, [props.authKey, props.base, teamIdTrimmed]);
  const teamPrefsClientId = React.useMemo(() => String(props.clientId || "webui"), [props.clientId]);
  const teamPrefsBase = React.useMemo(() => String(props.base || "").trim(), [props.base]);
  const extractTeamEventsCursorByScope = React.useCallback((prefs: any): Record<string, TeamCursorEntry> => {
    if (!prefs || typeof prefs !== "object") return {};
    const raw = (prefs as any).team_events_cursor;
    if (!raw || typeof raw !== "object") return {};
    const byScope = (raw as any).by_scope;
    if (!byScope || typeof byScope !== "object") return {};
    const out: Record<string, TeamCursorEntry> = {};
    for (const [key, value] of Object.entries(byScope)) {
      if (!value || typeof value !== "object") continue;
      const cursor = (value as any).cursor_ts;
      if (typeof cursor !== "number" || !Number.isFinite(cursor) || cursor <= 0) continue;
      out[key] = {
        cursor_ts: cursor,
        updated_unix_ms: typeof (value as any).updated_unix_ms === "number" ? (value as any).updated_unix_ms : undefined,
      };
    }
    return out;
  }, []);
  React.useEffect(() => {
    teamEventsCursorRef.current = teamEventsCursor;
  }, [teamEventsCursor]);

  const pushTeamEventsCursor = React.useCallback(
    async (nextMap: Record<string, TeamCursorEntry>) => {
      if (!teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
      const payload = {
        client_id: teamPrefsClientId,
        client_kind: TEAM_EVENTS_PREFS_KIND,
        prefs: {
          team_events_cursor: {
            version: TEAM_EVENTS_PREFS_VERSION,
            by_scope: nextMap,
          },
        },
      };
      const resp = await apiBrokerPostClientPrefs(teamPrefsBase, payload, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events prefs update failed");
      }
      setTeamEventsCursorStatus("ready");
    },
    [props.auth, teamEventsScopeKey, teamPrefsBase, teamPrefsClientId],
  );

  const scheduleTeamEventsCursorPersist = React.useCallback(
    (nextMap: Record<string, TeamCursorEntry>) => {
      if (!teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
      if (teamEventsCursorStatus === "error") return;
      teamEventsCursorPersistRef.current.pending = nextMap;
      if (teamEventsCursorPersistRef.current.timer) return;
      teamEventsCursorPersistRef.current.timer = setTimeout(() => {
        const pending = teamEventsCursorPersistRef.current.pending;
        teamEventsCursorPersistRef.current.pending = null;
        teamEventsCursorPersistRef.current.timer = null;
        if (!pending) return;
        pushTeamEventsCursor(pending).catch(() => {
          setTeamEventsCursorStatus("error");
        });
      }, 1500);
    },
    [teamEventsCursorStatus, teamEventsScopeKey, teamPrefsBase, teamPrefsClientId, pushTeamEventsCursor],
  );

  const loadTeamEventsCursor = React.useCallback(async () => {
    if (!canQuery || !teamPrefsBase || !teamPrefsClientId || !teamEventsScopeKey) return;
    setTeamEventsCursorStatus("loading");
    try {
      const resp = await apiBrokerGetClientPrefs(teamPrefsBase, teamPrefsClientId, TEAM_EVENTS_PREFS_KIND, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events prefs fetch failed");
      }
      const remoteMap = extractTeamEventsCursorByScope(resp.prefs);
      setTeamEventsCursorByScope(remoteMap);
      const remoteTs = remoteMap[teamEventsScopeKey]?.cursor_ts || 0;
      const localTs = teamEventsCursorRef.current || 0;
      const merged = Math.max(localTs, remoteTs);
      if (merged > localTs && teamIdTrimmed) {
        setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: merged }));
      }
      if (merged > remoteTs) {
        const nextMap = {
          ...remoteMap,
          [teamEventsScopeKey]: { cursor_ts: merged, updated_unix_ms: Date.now() },
        };
        setTeamEventsCursorByScope(nextMap);
        scheduleTeamEventsCursorPersist(nextMap);
      }
      setTeamEventsCursorStatus("ready");
    } catch {
      setTeamEventsCursorStatus("error");
    }
  }, [
    canQuery,
    extractTeamEventsCursorByScope,
    props.auth,
    scheduleTeamEventsCursorPersist,
    teamEventsScopeKey,
    teamIdTrimmed,
    teamPrefsBase,
    teamPrefsClientId,
  ]);
  const membersList = Array.isArray(members) ? members : [];
  const rolePlanOptions = React.useMemo(() => {
    const set = new Set<string>();
    for (const member of membersList) {
      const role = String(member?.role || "").trim().toLowerCase();
      if (role) set.add(role);
    }
    try {
      const parsed = JSON.parse(String(teamEditRoleOverridesJson || "").trim() || "{}");
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        for (const key of Object.keys(parsed)) {
          const role = String(key || "").trim().toLowerCase();
          if (role) set.add(role);
        }
      }
    } catch {
      // ignore invalid JSON; handled elsewhere
    }
    for (const role of Object.keys(teamRoleInstructions)) {
      const r = String(role || "").trim().toLowerCase();
      if (r) set.add(r);
    }
    for (const edge of teamRoleGraphEdges) {
      const from = String(edge?.from_role || "").trim().toLowerCase();
      const to = String(edge?.to_role || "").trim().toLowerCase();
      if (from) set.add(from);
      if (to) set.add(to);
    }
    return Array.from(set).filter(Boolean).sort();
  }, [membersList, teamEditRoleOverridesJson, teamRoleInstructions, teamRoleGraphEdges]);
  const rulesList = Array.isArray(rules) ? rules : [];
  const memberAgentOptions = Array.isArray(memberAgents) ? memberAgents : [];
  const memberSelectedAgent = memberAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(memberAgentId || "").trim(),
  );
  const memberAgentDeployments = Array.isArray(memberSelectedAgent?.deployments)
    ? (memberSelectedAgent?.deployments as any[])
    : [];
  const memberEditSelectedAgent = memberAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(memberEditAgentId || "").trim(),
  );
  const memberEditAgentDeployments = Array.isArray(memberEditSelectedAgent?.deployments)
    ? (memberEditSelectedAgent?.deployments as any[])
    : [];
  const eventTeamId = React.useCallback((row?: BrokerEventRow | null) => {
    const payload = row?.payload;
    if (!payload || typeof payload !== "object") return "";
    return String((payload as any).team_id || "");
  }, []);

  const isTeamEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!TEAM_RUN_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const isGuidanceEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!GUIDANCE_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const isOrchestratorEvent = React.useCallback(
    (row?: BrokerEventRow | null) => {
      if (!row) return false;
      const type = String(row.type || "");
      if (!ORCHESTRATOR_EVENT_TYPES.has(type)) return false;
      if (!teamIdTrimmed) return true;
      return eventTeamId(row) === teamIdTrimmed;
    },
    [eventTeamId, teamIdTrimmed],
  );

  const buildEventKey = (row: BrokerEventRow) =>
    row.event_id || `${row.type || ""}:${row.ts_unix_ms || 0}:${row.trace_id || ""}`;

  const mergeTeamEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isTeamEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isTeamEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      if (out.length > TEAM_EVENTS_MAX) {
        return out.slice(out.length - TEAM_EVENTS_MAX);
      }
      return out;
    },
    [isTeamEvent],
  );

  const mergeOrchestratorEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isOrchestratorEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isOrchestratorEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      if (out.length > TEAM_EVENTS_MAX) {
        return out.slice(out.length - TEAM_EVENTS_MAX);
      }
      return out;
    },
    [isOrchestratorEvent],
  );

  const mergeGuidanceEvents = React.useCallback(
    (live: BrokerEventRow[], replay: BrokerEventRow[]) => {
      const seen = new Set<string>();
      const out: BrokerEventRow[] = [];
      const push = (row: BrokerEventRow) => {
        const key = buildEventKey(row);
        if (seen.has(key)) return;
        seen.add(key);
        out.push(row);
      };
      for (const row of replay) {
        if (!isGuidanceEvent(row)) continue;
        push(row);
      }
      for (const row of live) {
        if (!isGuidanceEvent(row)) continue;
        push(row);
      }
      out.sort((a, b) => (a.ts_unix_ms || 0) - (b.ts_unix_ms || 0));
      if (out.length > TEAM_EVENTS_MAX) {
        return out.slice(out.length - TEAM_EVENTS_MAX);
      }
      return out;
    },
    [isGuidanceEvent],
  );

  const liveEvents = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
  const mergedTeamEvents = React.useMemo(
    () => mergeTeamEvents(liveEvents, teamReplayEvents),
    [liveEvents, teamReplayEvents, mergeTeamEvents],
  );
  const mergedOrchestratorEvents = React.useMemo(
    () => mergeOrchestratorEvents(liveEvents, orchestratorReplayEvents),
    [liveEvents, orchestratorReplayEvents, mergeOrchestratorEvents],
  );
  const mergedGuidanceEvents = React.useMemo(
    () => mergeGuidanceEvents(liveEvents, teamReplayEvents),
    [liveEvents, teamReplayEvents, mergeGuidanceEvents],
  );

  React.useEffect(() => {
    void loadTeamEventsCursor();
  }, [loadTeamEventsCursor]);

  const loadTeamReplay = React.useCallback(async () => {
    if (!canQuery || !teamIdTrimmed) return;
    setTeamReplayBusy(true);
    setTeamReplayError(null);
    setTeamReplayNote(null);
    try {
      const resp = await apiBrokerEventsReplay(props.base, props.auth, {
        sinceTs: teamEventsCursorRef.current || 0,
        limit: TEAM_EVENTS_MAX,
        types: Array.from(new Set([...TEAM_RUN_EVENT_TYPES, ...ORCHESTRATOR_EVENT_TYPES, ...GUIDANCE_EVENT_TYPES])),
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team events replay failed");
      }
      const items = Array.isArray(resp.events) ? resp.events : [];
      const rows = items
        .map((ev: any) => ({
          type: String(ev?.type || ""),
          ts_unix_ms: typeof ev?.ts_unix_ms === "number" ? ev.ts_unix_ms : undefined,
          event_id: ev?.event_id ? String(ev.event_id) : undefined,
          trace_id: ev?.trace_id ? String(ev.trace_id) : undefined,
          payload: ev?.payload && typeof ev.payload === "object" ? (ev.payload as Record<string, any>) : undefined,
        }));
      setTeamReplayEvents(rows.filter((row) => isTeamEvent(row)));
      setOrchestratorReplayEvents(rows.filter((row) => isOrchestratorEvent(row)));
      let nextCursor = teamEventsCursorRef.current || 0;
      if (typeof resp.next_since_ts === "number") {
        nextCursor = Math.max(nextCursor, resp.next_since_ts);
      }
      for (const row of rows) {
        if (row.ts_unix_ms && row.ts_unix_ms > nextCursor) nextCursor = row.ts_unix_ms;
      }
      if (teamIdTrimmed && nextCursor > (teamEventsCursorByTeam[teamIdTrimmed] || 0)) {
        setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: nextCursor }));
      }
      setTeamReplayNote(`replay +${rows.length}`);
    } catch (err) {
      setTeamReplayError(String(err));
    } finally {
      setTeamReplayBusy(false);
    }
  }, [
    canQuery,
    teamIdTrimmed,
    props.base,
    props.auth,
    isTeamEvent,
    teamEventsCursorByTeam,
    setTeamEventsCursorByTeam,
  ]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setTeamReplayEvents([]);
      setTeamReplayError(null);
      setTeamReplayNote(null);
      return;
    }
    void loadTeamReplay();
  }, [teamIdTrimmed, loadTeamReplay]);

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    const rows = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
    let maxTs = teamEventsCursorRef.current || 0;
    for (const row of rows) {
      if (!isTeamEvent(row)) continue;
      const ts = row.ts_unix_ms || 0;
      if (ts > maxTs) maxTs = ts;
    }
    if (maxTs > (teamEventsCursorByTeam[teamIdTrimmed] || 0)) {
      setTeamEventsCursorByTeam((prev) => ({ ...prev, [teamIdTrimmed]: maxTs }));
    }
  }, [props.quorumEvents, teamIdTrimmed, isTeamEvent, teamEventsCursorByTeam, setTeamEventsCursorByTeam]);

  React.useEffect(() => {
    if (!teamEventsScopeKey || teamEventsCursor <= 0) return;
    const current = teamEventsCursorByScope[teamEventsScopeKey]?.cursor_ts || 0;
    if (teamEventsCursor <= current) return;
    const nextMap = {
      ...teamEventsCursorByScope,
      [teamEventsScopeKey]: { cursor_ts: teamEventsCursor, updated_unix_ms: Date.now() },
    };
    setTeamEventsCursorByScope(nextMap);
    scheduleTeamEventsCursorPersist(nextMap);
  }, [
    scheduleTeamEventsCursorPersist,
    teamEventsCursor,
    teamEventsCursorByScope,
    teamEventsScopeKey,
  ]);

  const refreshTeams = async () => {
    if (!canQuery) return;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      const resp = await apiBrokerTeamList(props.base, props.auth);
      const rows = Array.isArray(resp?.teams) ? resp.teams : [];
      setTeams(rows);
      if (!teamIdTrimmed && rows.length > 0) {
        setTeamId(String(rows[0]?.team_id || ""));
      }
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const refreshTeamDetails = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    try {
      const resp = await apiBrokerTeamGet(props.base, tid, props.auth);
      setTeamDetails(resp?.team ?? null);
    } catch {
      setTeamDetails(null);
    }
  };

  const refreshMembers = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMembersList(props.base, tid, props.auth);
      setMembers(Array.isArray(resp?.members) ? resp.members : []);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const refreshMemberAgents = async () => {
    if (!canQuery) return;
    setMemberAgentsError(null);
    setMemberAgentsBusy(true);
    try {
      const resp = await apiBrokerListAgents(props.base, props.auth);
      const rows = Array.isArray(resp?.agents) ? resp.agents : [];
      setMemberAgents(rows);
    } catch (err) {
      setMemberAgentsError(String(err));
    } finally {
      setMemberAgentsBusy(false);
    }
  };

  const refreshRules = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      const resp = await apiBrokerTeamQuorumList(props.base, tid, props.auth);
      setRules(Array.isArray(resp?.rules) ? resp.rules : []);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  const handleCreateTeam = async () => {
    const tid = String(newTeamId || "").trim();
    if (!tid) {
      setTeamsError("team_id required");
      return;
    }
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamCreate(props.base, { team_id: tid, display_name: String(newTeamName || "").trim() }, props.auth);
      setNewTeamId("");
      setNewTeamName("");
      await refreshTeams();
      setTeamId(tid);
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const slugifyTeamId = (name: string): string => {
    const base = String(name || "")
      .trim()
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "");
    return base || `team-${Date.now()}`;
  };

  const handleQuickBuilderApplyTemplate = React.useCallback(
    (template: string) => {
      setQuickTemplate(template);
      setQuickMembers(makeQuickMembers(template));
    },
    [makeQuickMembers],
  );

  const handleQuickMemberUpdate = (id: string, patch: Partial<QuickMember>) => {
    setQuickMembers((prev) =>
      prev.map((m) => {
        if (m.id !== id) return m;
        const next = { ...m, ...patch };
        if (patch.provider !== undefined) {
          const def = providerDefaults[patch.provider] ?? "";
          const modelDef = providerModelDefaults[patch.provider] ?? "";
          if (!next.baseUrl || next.baseUrl === providerDefaults[m.provider]) {
            next.baseUrl = def;
          }
          if (!next.model || next.model === providerModelDefaults[m.provider]) {
            next.model = modelDef;
          }
        }
        return next;
      }),
    );
  };

  const handleQuickAddMember = () => {
    setQuickMembers((prev) => [
      ...prev,
      {
        id: `member-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
        role: "executor",
        provider: "openai",
        model: providerModelDefaults.openai,
        baseUrl: providerDefaults.openai,
        agentId: "",
        deploymentId: "",
      },
    ]);
  };

  const handleQuickRemoveMember = (id: string) => {
    setQuickMembers((prev) => prev.filter((m) => m.id !== id));
  };

  const handleQuickCreateTeam = async () => {
    if (!canQuery) return;
    setQuickBuilderError(null);
    setQuickBuilderBusy(true);
    try {
      const name = String(quickTeamName || "").trim();
      if (!name) throw new Error("team name required");
      const id = String(quickTeamId || "").trim() || slugifyTeamId(name);
      const goal = String(quickTeamGoal || "").trim();
      const meta: Record<string, any> = {};
      if (goal) meta.goal = goal;
      const payload: Record<string, any> = { team_id: id, display_name: name };
      if (Object.keys(meta).length > 0) payload.meta = meta;
      const createResp = await apiBrokerTeamCreate(props.base, payload, props.auth);
      if (!createResp.ok) throw new Error(createResp.error || createResp.err || createResp.code || "team create failed");

      for (const member of quickMembers) {
        const role = String(member.role || "").trim();
        if (!role) continue;
        const memberPayload: Record<string, any> = { role };
        const agentId = String(member.agentId || "").trim();
        const deploymentId = String(member.deploymentId || "").trim();
        if (agentId) memberPayload.agent_id = agentId;
        if (deploymentId) memberPayload.deployment_id = deploymentId;
        const metaMember: Record<string, any> = {};
        const provider = String(member.provider || "").trim();
        if (provider) metaMember.provider = provider;
        const runOverrides: Record<string, any> = {};
        const model = String(member.model || "").trim();
        if (model) runOverrides.model = model;
        const baseUrl = String(member.baseUrl || "").trim();
        if (baseUrl) runOverrides.base_url = baseUrl;
        if (Object.keys(runOverrides).length > 0) metaMember.run_overrides = runOverrides;
        if (Object.keys(metaMember).length > 0) memberPayload.meta = metaMember;
        const upsertResp = await apiBrokerTeamMembersUpsert(props.base, id, memberPayload, props.auth);
        if (!upsertResp.ok) {
          throw new Error(upsertResp.error || upsertResp.err || upsertResp.code || "member create failed");
        }
      }

      setQuickTeamId(id);
      setTeamId(id);
      await refreshTeams();
      await refreshTeamDetails(id);
      await refreshMembers(id);
    } catch (err) {
      setQuickBuilderError(String(err));
    } finally {
      setQuickBuilderBusy(false);
    }
  };

  const handleDeleteTeam = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (!window.confirm(`Delete team "${tid}"?`)) return;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamDelete(props.base, tid, props.auth);
      setTeamId("");
      setTeamDetails(null);
      await refreshTeams();
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const loadTeamEditsFromDetails = (details: any | null) => {
    if (!details) {
      setTeamEditName("");
      setTeamEditTags("");
      setTeamEditPolicyRef("");
      setTeamEditSharedScope("");
      setTeamEditSharedMode("read_write");
      setTeamEditMetaJson("");
      setTeamEditRoleOverridesJson("");
      return;
    }
    setTeamEditName(String(details?.display_name || ""));
    const tags = Array.isArray(details?.tags) ? details.tags : [];
    setTeamEditTags(tags.map((t: any) => String(t)).filter(Boolean).join(", "));
    setTeamEditPolicyRef(String(details?.policy_ref || ""));
    setTeamEditSharedScope(String(details?.shared_memory_scope_id || ""));
    if (details?.meta && typeof details.meta === "object") {
      try {
        setTeamEditMetaJson(JSON.stringify(details.meta, null, 2));
      } catch {
        setTeamEditMetaJson("");
      }
    } else {
      setTeamEditMetaJson("");
    }
    const metaObj = details?.meta && typeof details.meta === "object" ? (details.meta as Record<string, any>) : null;
    setTeamEditSharedMode(
      normalizeSharedMemoryMode(metaObj?.shared_memory_mode ?? metaObj?.memory_scope_mode ?? "read_write"),
    );
    if (metaObj?.role_overrides && typeof metaObj.role_overrides === "object") {
      try {
        setTeamEditRoleOverridesJson(JSON.stringify(metaObj.role_overrides, null, 2));
      } catch {
        setTeamEditRoleOverridesJson("");
      }
    } else {
      setTeamEditRoleOverridesJson("");
    }
    if (metaObj) {
      setTeamRoleInstructions(normalizeRoleInstructionMap(metaObj.role_instructions));
      setTeamRolePromptMode(normalizeRolePromptMode(metaObj.role_prompt_mode));
      setTeamRoleGraphEdges(normalizeRoleGraphEdges(metaObj.role_graph));
    } else {
      setTeamRoleInstructions({});
      setTeamRolePromptMode("prepend");
      setTeamRoleGraphEdges([]);
    }
    setTeamRolePlanTouched(false);
  };

  const handleRoleInstructionsChange = (next: Record<string, string>) => {
    setTeamRolePlanTouched(true);
    setTeamRoleInstructions(next);
  };

  const handleRolePromptModeChange = (next: string) => {
    setTeamRolePlanTouched(true);
    setTeamRolePromptMode(next);
  };

  const handleRoleGraphEdgesChange = (next: RoleGraphEdge[]) => {
    setTeamRolePlanTouched(true);
    setTeamRoleGraphEdges(next);
  };

  const handleUpdateTeam = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const displayName = String(teamEditName || "").trim();
    if (!displayName) {
      setTeamEditError("display_name required");
      return;
    }
    const tagsRaw = String(teamEditTags || "").trim();
    const tags =
      tagsRaw.length === 0
        ? []
        : tagsRaw
            .split(",")
            .map((t) => t.trim())
            .filter(Boolean);
    const policyRef = String(teamEditPolicyRef || "").trim();
    const sharedScope = String(teamEditSharedScope || "").trim();
    const sharedModeRaw = String(teamEditSharedMode || "").trim().toLowerCase();
    if (
      sharedModeRaw &&
      sharedModeRaw !== "read_only" &&
      sharedModeRaw !== "read_write" &&
      sharedModeRaw !== "readonly" &&
      sharedModeRaw !== "readwrite"
    ) {
      setTeamEditError("shared memory mode must be read_only or read_write");
      return;
    }
    const sharedMode = normalizeSharedMemoryMode(sharedModeRaw);
    const metaRaw = String(teamEditMetaJson || "").trim();
    let meta: Record<string, any> = {};
    if (metaRaw) {
      try {
        const parsed = JSON.parse(metaRaw);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          meta = parsed;
        } else {
          setTeamEditError("meta must be a JSON object");
          return;
        }
      } catch (err) {
        setTeamEditError(`invalid meta json: ${String(err)}`);
        return;
      }
    }
    const roleOverridesRaw = String(teamEditRoleOverridesJson || "").trim();
    if (roleOverridesRaw) {
      try {
        const parsed = JSON.parse(roleOverridesRaw);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          meta.role_overrides = parsed;
        } else {
          setTeamEditError("role overrides must be a JSON object keyed by role");
          return;
        }
      } catch (err) {
        setTeamEditError(`invalid role overrides json: ${String(err)}`);
        return;
      }
    }
    if (teamRolePlanTouched) {
      if (Object.keys(teamRoleInstructions).length > 0) {
        meta.role_instructions = teamRoleInstructions;
        meta.role_prompt_mode = normalizeRolePromptMode(teamRolePromptMode);
      } else {
        delete meta.role_instructions;
        delete meta.role_prompt_mode;
      }
      if (teamRoleGraphEdges.length > 0) {
        meta.role_graph = { edges: teamRoleGraphEdges };
      } else {
        delete meta.role_graph;
      }
    }
    if (sharedScope) {
      meta.shared_memory_mode = sharedMode || "read_write";
    } else {
      delete meta.shared_memory_mode;
    }
    setTeamEditError(null);
    setTeamEditBusy(true);
    try {
      const resp = await apiBrokerTeamUpdate(
        props.base,
        tid,
        {
          display_name: displayName,
          tags,
          policy_ref: policyRef,
          shared_memory_scope_id: sharedScope,
          meta,
        },
        props.auth,
      );
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "update team failed");
      }
      setTeamDetails(resp?.team ?? null);
      await refreshTeams();
      loadTeamEditsFromDetails(resp?.team ?? null);
    } catch (err) {
      setTeamEditError(String(err));
    } finally {
      setTeamEditBusy(false);
    }
  };

  const handleAddMember = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const role = String(memberRole || "").trim();
    if (!role) {
      setMembersError("role required");
      return;
    }
    setMembersError(null);
    setMembersBusy(true);
    try {
      const payload: Record<string, any> = { role };
      const mid = String(memberId || "").trim();
      if (mid) payload.member_id = mid;
      const aid = String(memberAgentId || "").trim();
      if (aid) payload.agent_id = aid;
      const dep = String(memberDeploymentId || "").trim();
      if (dep) payload.deployment_id = dep;
      const status = String(memberStatus || "").trim();
      if (status) payload.status = status;
      const w = Number.parseInt(String(memberWeight || ""), 10);
      if (Number.isFinite(w)) payload.weight = w;
      const caps = String(memberCapabilities || "")
        .split(",")
        .map((c) => c.trim())
        .filter(Boolean);
      if (caps.length > 0) payload.capabilities = caps;
      const meta: Record<string, any> = {};
      const backendLabel = String(memberBackendLabel || "").trim();
      if (backendLabel) meta.backend_label = backendLabel;
      const runOverrides: Record<string, any> = {};
      const model = String(memberModel || "").trim();
      if (model) runOverrides.model = model;
      const baseUrl = String(memberBaseUrl || "").trim();
      if (baseUrl) runOverrides.base_url = baseUrl;
      const summaryModel = String(memberSummaryModel || "").trim();
      if (summaryModel) runOverrides.summary_model = summaryModel;
      const tools = String(memberTools || "").trim();
      if (tools) runOverrides.tools = tools;
      const timeoutMs = Number.parseInt(String(memberTimeoutMs || "").trim(), 10);
      if (Number.isFinite(timeoutMs)) runOverrides.timeout_ms = timeoutMs;
      if (Object.keys(runOverrides).length > 0) meta.run_overrides = runOverrides;
      if (Object.keys(meta).length > 0) payload.meta = meta;
      await apiBrokerTeamMembersUpsert(props.base, tid, payload, props.auth);
      setMemberId("");
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleToggleMemberStatus = async (member: TeamMemberRow) => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const mid = String(member?.member_id || "").trim();
    if (!mid) return;
    const current = String(member?.status || "active").toLowerCase();
    const next = current === "paused" ? "active" : "paused";
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMemberUpdate(props.base, tid, mid, { status: next }, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "update member failed");
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleSetAllMemberStatus = async (status: "active" | "paused") => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (membersList.length === 0) {
      setMembersError("no team members loaded");
      return;
    }
    if (!window.confirm(`Set ${membersList.length} member(s) to ${status}?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const member of membersList) {
        const mid = String(member?.member_id || "").trim();
        if (!mid) continue;
        const current = String(member?.status || "active").toLowerCase();
        if (current === status) continue;
        const resp = await apiBrokerTeamMemberUpdate(props.base, tid, mid, { status }, props.auth);
        if (!resp.ok) {
          throw new Error(resp.error || resp.err || resp.code || "update member failed");
        }
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleRemovePausedMembers = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const paused = membersList.filter(
      (m) => String(m?.status || "").trim().toLowerCase() === "paused",
    );
    if (paused.length === 0) {
      setMembersError("no paused members to remove");
      return;
    }
    if (!window.confirm(`Remove ${paused.length} paused member(s) from the team?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const member of paused) {
        const mid = String(member?.member_id || "").trim();
        if (!mid) continue;
        await apiBrokerTeamMembersDelete(props.base, tid, mid, props.auth);
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleEditMember = (member: TeamMemberRow) => {
    const mid = String(member?.member_id || "").trim();
    if (!mid) return;
    setMemberEditId(mid);
    setMemberEditRole(String(member?.role || ""));
    setMemberEditStatus(String(member?.status || "active"));
    setMemberEditAgentId(String(member?.agent_id || ""));
    setMemberEditDeploymentId(String(member?.deployment_id || ""));
    setMemberEditWeight(
      typeof member?.weight === "number" && Number.isFinite(member.weight) ? String(member.weight) : "",
    );
    const caps = Array.isArray(member?.capabilities)
      ? member.capabilities.map((c) => String(c).trim()).filter(Boolean)
      : [];
    setMemberEditCapabilities(caps.join(", "));
    let metaObj: Record<string, any> | null = null;
    if (member?.meta && typeof member.meta === "object") {
      metaObj = member.meta as Record<string, any>;
      try {
        setMemberEditMetaJson(JSON.stringify(member.meta, null, 2));
      } catch {
        setMemberEditMetaJson("");
      }
    } else {
      setMemberEditMetaJson("");
    }
    const backendLabel = metaObj?.backend_label ? String(metaObj.backend_label) : "";
    setMemberEditBackendLabel(backendLabel);
    const overridesRaw =
      metaObj?.run_overrides && typeof metaObj.run_overrides === "object"
        ? (metaObj.run_overrides as Record<string, any>)
        : null;
    setMemberEditModel(overridesRaw?.model ? String(overridesRaw.model) : "");
    setMemberEditBaseUrl(overridesRaw?.base_url ? String(overridesRaw.base_url) : "");
    setMemberEditSummaryModel(overridesRaw?.summary_model ? String(overridesRaw.summary_model) : "");
    setMemberEditTools(overridesRaw?.tools ? String(overridesRaw.tools) : "");
    const timeoutRaw = overridesRaw?.timeout_ms;
    if (typeof timeoutRaw === "number" && Number.isFinite(timeoutRaw)) {
      setMemberEditTimeoutMs(String(timeoutRaw));
    } else if (typeof timeoutRaw === "string" && timeoutRaw.trim().length > 0) {
      setMemberEditTimeoutMs(timeoutRaw.trim());
    } else {
      setMemberEditTimeoutMs("");
    }
    setMemberEditError(null);
  };

  const handleCancelMemberEdit = () => {
    setMemberEditId("");
    setMemberEditRole("");
    setMemberEditStatus("");
    setMemberEditAgentId("");
    setMemberEditDeploymentId("");
    setMemberEditWeight("");
    setMemberEditCapabilities("");
    setMemberEditMetaJson("");
    setMemberEditBackendLabel("");
    setMemberEditModel("");
    setMemberEditBaseUrl("");
    setMemberEditSummaryModel("");
    setMemberEditTools("");
    setMemberEditTimeoutMs("");
    setMemberEditError(null);
  };

  const handleSaveMemberEdit = async () => {
    const tid = teamIdTrimmed;
    const mid = String(memberEditId || "").trim();
    if (!tid || !mid) return;
    const role = String(memberEditRole || "").trim();
    if (!role) {
      setMemberEditError("role required");
      return;
    }
    const status = String(memberEditStatus || "").trim() || "active";
    const agentId = String(memberEditAgentId || "").trim();
    const deploymentId = String(memberEditDeploymentId || "").trim();
    const caps = String(memberEditCapabilities || "")
      .split(",")
      .map((c) => c.trim())
      .filter(Boolean);
    const weightRaw = String(memberEditWeight || "").trim();
    let weightValue: number | undefined = undefined;
    if (weightRaw.length > 0) {
      const parsed = Number.parseInt(weightRaw, 10);
      if (!Number.isFinite(parsed)) {
        setMemberEditError("weight must be a number");
        return;
      }
      weightValue = parsed;
    }
    let meta: Record<string, any> = {};
    let hasMeta = false;
    const metaRaw = String(memberEditMetaJson || "").trim();
    if (metaRaw) {
      try {
        const parsed = JSON.parse(metaRaw);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setMemberEditError("meta must be a JSON object");
          return;
        }
        meta = parsed as Record<string, any>;
        hasMeta = true;
      } catch (err) {
        setMemberEditError(`invalid meta json: ${String(err)}`);
        return;
      }
    }
    const backendLabel = String(memberEditBackendLabel || "").trim();
    if (backendLabel) {
      meta.backend_label = backendLabel;
      hasMeta = true;
    } else if (meta && Object.prototype.hasOwnProperty.call(meta, "backend_label")) {
      delete meta.backend_label;
      hasMeta = true;
    }
    const runOverrides: Record<string, any> =
      meta && meta.run_overrides && typeof meta.run_overrides === "object" ? { ...meta.run_overrides } : {};
    const model = String(memberEditModel || "").trim();
    if (model) runOverrides.model = model;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "model")) delete runOverrides.model;
    const baseUrl = String(memberEditBaseUrl || "").trim();
    if (baseUrl) runOverrides.base_url = baseUrl;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "base_url")) delete runOverrides.base_url;
    const summaryModel = String(memberEditSummaryModel || "").trim();
    if (summaryModel) runOverrides.summary_model = summaryModel;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "summary_model")) delete runOverrides.summary_model;
    const tools = String(memberEditTools || "").trim();
    if (tools) runOverrides.tools = tools;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "tools")) delete runOverrides.tools;
    const timeoutMsRaw = String(memberEditTimeoutMs || "").trim();
    if (timeoutMsRaw.length > 0) {
      const parsed = Number.parseInt(timeoutMsRaw, 10);
      if (!Number.isFinite(parsed)) {
        setMemberEditError("timeout_ms must be a number");
        return;
      }
      runOverrides.timeout_ms = parsed;
    } else if (Object.prototype.hasOwnProperty.call(runOverrides, "timeout_ms")) {
      delete runOverrides.timeout_ms;
    }
    if (Object.keys(runOverrides).length > 0) {
      meta.run_overrides = runOverrides;
      hasMeta = true;
    } else if (meta && Object.prototype.hasOwnProperty.call(meta, "run_overrides")) {
      delete meta.run_overrides;
      hasMeta = true;
    }
    setMemberEditError(null);
    setMemberEditBusy(true);
    try {
      const payload: Record<string, any> = {
        role,
        status,
        capabilities: caps,
      };
      if (hasMeta) payload.meta = meta;
      payload.agent_id = agentId;
      payload.deployment_id = deploymentId;
      if (weightValue !== undefined) payload.weight = weightValue;
      const resp = await apiBrokerTeamMemberUpdate(props.base, tid, mid, payload, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "update member failed");
      }
      await refreshMembers(tid);
      handleCancelMemberEdit();
    } catch (err) {
      setMemberEditError(String(err));
    } finally {
      setMemberEditBusy(false);
    }
  };

  const handleAddConnectedAgentsToTeam = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (!canQuery) return;
    if (!memberRole.trim()) {
      setMembersError("role required");
      return;
    }
    const agentOptions = Array.isArray(memberAgents) ? memberAgents : [];
    if (agentOptions.length === 0) {
      setMembersError("load agents before bulk add");
      return;
    }
    const role = String(memberRole || "").trim() || "executor";
    const existingAgentIds = new Set<string>();
    for (const m of membersList) {
      const aid = String(m?.agent_id || "").trim();
      if (aid) existingAgentIds.add(aid);
    }
    const payloads: Record<string, any>[] = [];
    for (const agent of agentOptions) {
      const aid = String(agent?.agent_id || "").trim();
      if (!aid || existingAgentIds.has(aid)) continue;
      const deployments = Array.isArray(agent?.deployments) ? agent.deployments : [];
      const connected = agent?.connected === true || deployments.length > 0;
      if (!connected) continue;
      const payload: Record<string, any> = { role, agent_id: aid };
      if (deployments.length > 0) {
        const depId = deployments[0]?.deployment_id ? String(deployments[0].deployment_id) : "";
        if (depId) payload.deployment_id = depId;
      }
      payloads.push(payload);
      existingAgentIds.add(aid);
    }
    if (payloads.length === 0) {
      setMembersError("no connected agents to add (already in team)");
      return;
    }
    if (!window.confirm(`Add ${payloads.length} connected agent(s) to team?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const payload of payloads) {
        await apiBrokerTeamMembersUpsert(props.base, tid, payload, props.auth);
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleDeleteMember = async (memberIdRaw: string) => {
    const tid = teamIdTrimmed;
    const mid = String(memberIdRaw || "").trim();
    if (!tid || !mid) return;
    if (!window.confirm(`Remove member "${mid}"?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      await apiBrokerTeamMembersDelete(props.base, tid, mid, props.auth);
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleAddRule = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const action = String(ruleAction || "").trim();
    const min = Number.parseInt(String(ruleMinApprovals || ""), 10);
    const mode = String(ruleMode || "").trim();
    if (!action) {
      setRulesError("action required");
      return;
    }
    if (!Number.isFinite(min) || min <= 0) {
      setRulesError("min approvals must be > 0");
      return;
    }
    if (!mode) {
      setRulesError("quorum mode required");
      return;
    }
    setRulesError(null);
    setRulesBusy(true);
    try {
      await apiBrokerTeamQuorumUpsert(props.base, tid, { action, min_approvals: min, quorum_mode: mode }, props.auth);
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  const handleDeleteRule = async (ruleIdRaw: string) => {
    const tid = teamIdTrimmed;
    const rid = String(ruleIdRaw || "").trim();
    if (!tid || !rid) return;
    if (!window.confirm(`Delete quorum rule "${rid}"?`)) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      await apiBrokerTeamQuorumDelete(props.base, tid, rid, props.auth);
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  React.useEffect(() => {
    if (!canQuery || teamList.length === 0) return;
    if (!teamIdTrimmed) {
      setTeamId(String(teamList[0]?.team_id || ""));
      return;
    }
    void refreshTeamDetails(teamIdTrimmed);
  }, [canQuery, teamIdTrimmed, teamList.length]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      loadTeamEditsFromDetails(null);
      return;
    }
    loadTeamEditsFromDetails(teamDetails);
  }, [teamDetails, teamIdTrimmed]);

  React.useEffect(() => {
    if (!canQuery || !teamIdTrimmed) return;
    void refreshMembers(teamIdTrimmed);
    void refreshRules(teamIdTrimmed);
  }, [canQuery, teamIdTrimmed]);

  React.useEffect(() => {
    if (!canQuery) return;
    if (memberAgentsBusy || (memberAgents && memberAgents.length > 0)) return;
    void refreshMemberAgents();
  }, [canQuery, memberAgentsBusy, memberAgents, refreshMemberAgents]);

  React.useEffect(() => {
    if (!memberAgentId) {
      if (memberDeploymentId) {
        setMemberDeploymentId("");
      }
      return;
    }
    if (memberDeploymentId) return;
    if (memberAgentDeployments.length === 0) return;
    const first = memberAgentDeployments[0];
    const depId = first?.deployment_id ? String(first.deployment_id) : "";
    if (depId) {
      setMemberDeploymentId(depId);
    }
  }, [memberAgentId, memberDeploymentId, memberAgentDeployments]);

  React.useEffect(() => {
    if (!memberEditAgentId) {
      if (memberEditDeploymentId) {
        setMemberEditDeploymentId("");
      }
      return;
    }
    if (memberEditDeploymentId) return;
    if (memberEditAgentDeployments.length === 0) return;
    const first = memberEditAgentDeployments[0];
    const depId = first?.deployment_id ? String(first.deployment_id) : "";
    if (depId) {
      setMemberEditDeploymentId(depId);
    }
  }, [memberEditAgentId, memberEditDeploymentId, memberEditAgentDeployments]);

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Teams</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || teamsBusy}
            onClick={() => void refreshTeams()}
          >
            {teamsBusy ? "Loading…" : "Refresh"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || teamsBusy || !teamIdTrimmed}
            onClick={() => void handleDeleteTeam()}
          >
            Delete
          </button>
        </div>
      </div>
      <div className="mb-2 text-[11px] text-white/50">Manage team members, quorum rules, and team runs.</div>

      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Team</FieldLabel>
          <select
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            data-testid="team-select"
            value={teamIdTrimmed}
            onChange={(e) => setTeamId(e.target.value)}
            disabled={teamList.length === 0}
          >
            {teamList.length === 0 ? <option value="">(no teams)</option> : null}
            {teamList.map((t) => (
              <option key={String(t?.team_id)} value={String(t?.team_id)}>
                {String(t?.display_name || t?.team_id || "")}
              </option>
            ))}
          </select>
        </div>
        {teamDetails ? (
          <div className="text-[11px] text-white/50">
            owner {String(teamDetails?.owner_sub || "unknown")}
            {teamDetails?.created_unix_ms ? ` · ${fmtTs(teamDetails.created_unix_ms)}` : ""}
          </div>
        ) : null}
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2" data-testid="team-tabs">
        {teamTabs.map((tab) => {
          const active = tab.id === teamTab;
          return (
            <button
              key={tab.id}
              data-testid={`team-tab-${tab.id}`}
              className={`rounded-md px-3 py-1.5 text-xs ${
                active ? "bg-indigo-500/20 text-indigo-100" : "bg-black/20 text-white/70 hover:bg-black/30"
              }`}
              type="button"
              onClick={() => setTeamTab(tab.id)}
            >
              {tab.label}
            </button>
          );
        })}
      </div>

      {teamTab === "run" ? (
        <SectionCard title="Team run" description="Start and monitor team runs." defaultOpen={true}>
          <BrokerTeamRunPanel
            base={props.base}
            auth={props.auth}
            canQuery={canQuery}
            teamId={teamIdTrimmed}
            members={membersList}
            rules={rulesList}
            quorumEvents={mergedTeamEvents}
            teamMeta={teamDetails?.meta && typeof teamDetails.meta === "object" ? (teamDetails.meta as Record<string, any>) : null}
            onMembersRefresh={refreshMembers}
            onTeamSelect={setTeamId}
          />
        </SectionCard>
      ) : null}

      {teamTab === "settings" ? (
        <SectionCard title="Team settings" description="Name, tags, shared memory, and role graph defaults.">
          <BrokerTeamSettingsPanel
            canQuery={canQuery}
            teamId={teamIdTrimmed}
            teamDetails={teamDetails}
            teamEditName={teamEditName}
            teamEditTags={teamEditTags}
            teamEditPolicyRef={teamEditPolicyRef}
            teamEditSharedScope={teamEditSharedScope}
            teamEditSharedMode={teamEditSharedMode}
            teamEditMetaJson={teamEditMetaJson}
            teamEditRoleOverridesJson={teamEditRoleOverridesJson}
            teamRoleInstructions={teamRoleInstructions}
            teamRolePromptMode={teamRolePromptMode}
            teamRoleGraphEdges={teamRoleGraphEdges}
            rolePlanOptions={rolePlanOptions}
            teamEditBusy={teamEditBusy}
            teamEditError={teamEditError}
            onLoadTeamEdits={() => loadTeamEditsFromDetails(teamDetails)}
            onTeamEditNameChange={setTeamEditName}
            onTeamEditTagsChange={setTeamEditTags}
            onTeamEditPolicyRefChange={setTeamEditPolicyRef}
            onTeamEditSharedScopeChange={setTeamEditSharedScope}
            onTeamEditSharedModeChange={setTeamEditSharedMode}
            onTeamEditMetaJsonChange={setTeamEditMetaJson}
            onTeamEditRoleOverridesJsonChange={setTeamEditRoleOverridesJson}
            onRoleInstructionsChange={handleRoleInstructionsChange}
            onRolePromptModeChange={handleRolePromptModeChange}
            onRoleGraphEdgesChange={handleRoleGraphEdgesChange}
            onUpdateTeam={() => void handleUpdateTeam()}
          />
        </SectionCard>
      ) : null}

      {teamTab === "setup" ? (
        <>
          <SectionCard
            title="Quick team builder"
            description="Create a team and add agents in one step."
            defaultOpen={true}
          >
            <div className="text-[11px] text-white/60">
              Defaults stay simple; advanced fields are optional.
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Team name</FieldLabel>
              <input
                className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={quickTeamName}
                onChange={(e) => setQuickTeamName(e.target.value)}
                placeholder="Ops team"
              />
              <FieldLabel>Team id</FieldLabel>
              <input
                className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={quickTeamId}
                onChange={(e) => setQuickTeamId(e.target.value)}
                placeholder="auto"
              />
              <FieldLabel>Goal</FieldLabel>
              <input
                className="min-w-[260px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={quickTeamGoal}
                onChange={(e) => setQuickTeamGoal(e.target.value)}
                placeholder="What is this team trying to accomplish?"
              />
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Template</FieldLabel>
              <select
                className="min-w-[180px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={quickTemplate}
                onChange={(e) => handleQuickBuilderApplyTemplate(e.target.value)}
              >
                <option value="standard">Planner + Executor + Critic</option>
                <option value="planner_executor">Planner + Executor</option>
                <option value="research_team">Researcher + Executor + Critic</option>
              </select>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={handleQuickAddMember}
              >
                Add agent
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                disabled={!canQuery || memberAgentsBusy}
                onClick={() => void refreshMemberAgents()}
              >
                {memberAgentsBusy ? "Refreshing agents…" : "Refresh agents"}
              </button>
            </div>

            <div className="grid gap-2">
              {quickMembers.map((m) => (
                <div key={m.id} className="rounded-md border border-white/10 bg-black/20 p-2">
                  <div className="flex flex-wrap items-center gap-2">
                    <FieldLabel>Role</FieldLabel>
                    <input
                      className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.role}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { role: e.target.value })}
                    />
                    <FieldLabel>Provider</FieldLabel>
                    <select
                      className="min-w-[140px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.provider}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { provider: e.target.value })}
                    >
                      <option value="openai">OpenAI</option>
                      <option value="anthropic">Anthropic</option>
                      <option value="deepseek">DeepSeek</option>
                      <option value="kimi">Kimi (Moonshot)</option>
                      <option value="glm">GLM (Zhipu)</option>
                      <option value="local">Local</option>
                      <option value="custom">Custom</option>
                    </select>
                    <FieldLabel>Model</FieldLabel>
                    <input
                      className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.model}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { model: e.target.value })}
                      placeholder={providerModelDefaults[m.provider] || "model"}
                    />
                    <FieldLabel>Base URL</FieldLabel>
                    <input
                      className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.baseUrl}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { baseUrl: e.target.value })}
                      placeholder="https://api.openai.com/v1"
                    />
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => handleQuickRemoveMember(m.id)}
                      disabled={quickMembers.length <= 1}
                    >
                      Remove
                    </button>
                  </div>
                  <div className="mt-2 flex flex-wrap items-center gap-2">
                    <FieldLabel>Agent</FieldLabel>
                    <select
                      className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.agentId}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { agentId: e.target.value, deploymentId: "" })}
                    >
                      <option value="">(any)</option>
                      {(memberAgents || []).map((agent: any) => (
                        <option key={agent?.agent_id || agent?.id} value={String(agent?.agent_id || agent?.id || "")}>
                          {String(agent?.display_name || agent?.agent_id || agent?.id || "")}
                        </option>
                      ))}
                    </select>
                    <FieldLabel>Deployment</FieldLabel>
                    <input
                      className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.deploymentId}
                      onChange={(e) => handleQuickMemberUpdate(m.id, { deploymentId: e.target.value })}
                      placeholder="optional"
                    />
                  </div>
                </div>
              ))}
            </div>

            {quickBuilderError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                {quickBuilderError}
              </div>
            ) : null}
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-indigo-500/20 px-3 py-2 text-xs font-semibold text-indigo-100 hover:bg-indigo-500/30 disabled:opacity-50"
                type="button"
                disabled={!canQuery || quickBuilderBusy}
                onClick={() => void handleQuickCreateTeam()}
              >
                {quickBuilderBusy ? "Creating…" : "Create team + add agents"}
              </button>
              {teamsBusy ? <span className="text-xs text-white/50">Refreshing teams…</span> : null}
            </div>
          </SectionCard>

          <SectionCard title="Create team" description="Manual team creation if you want a minimal team first.">
            <BrokerTeamCreatePanel
              canQuery={canQuery}
              teamsBusy={teamsBusy}
              newTeamId={newTeamId}
              newTeamName={newTeamName}
              onNewTeamIdChange={setNewTeamId}
              onNewTeamNameChange={setNewTeamName}
              onCreateTeam={() => void handleCreateTeam()}
            />
          </SectionCard>
        </>
      ) : null}

      {teamTab === "members" ? (
        <SectionCard title="Team members" description="Add members, update status, or edit overrides.">
        <div className="flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Team members</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy}
            onClick={() => void refreshMembers(teamIdTrimmed)}
          >
            {membersBusy ? "Loading…" : "Refresh"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberId}
            onChange={(e) => setMemberId(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Role</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberRole}
            onChange={(e) => setMemberRole(e.target.value)}
          />
          <FieldLabel>Status</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberStatus}
            onChange={(e) => setMemberStatus(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Agent ID</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberAgentId}
            onChange={(e) => setMemberAgentId(e.target.value)}
            placeholder="agent1"
          />
          <FieldLabel>Deployment</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberDeploymentId}
            onChange={(e) => setMemberDeploymentId(e.target.value)}
            placeholder="default"
          />
          <FieldLabel>Weight</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberWeight}
            onChange={(e) => setMemberWeight(e.target.value)}
          />
          <FieldLabel>Capabilities</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberCapabilities}
            onChange={(e) => setMemberCapabilities(e.target.value)}
            placeholder="vision,audio"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Agent pick</FieldLabel>
          <select
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberAgentId}
            onChange={(e) => setMemberAgentId(e.target.value)}
          >
            <option value="">(select agent)</option>
            {memberAgentOptions.map((agent) => {
              const aid = String(agent?.agent_id || "");
              if (!aid) return null;
              const suffix = agent?.connected ? " · connected" : "";
              return (
                <option key={`member-agent-${aid}`} value={aid}>
                  {aid}
                  {suffix}
                </option>
              );
            })}
          </select>
          <FieldLabel>Deployment pick</FieldLabel>
          <select
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberDeploymentId}
            onChange={(e) => setMemberDeploymentId(e.target.value)}
            disabled={memberAgentDeployments.length === 0}
          >
            <option value="">(default)</option>
            {memberAgentDeployments.map((dep, idx) => {
              const depId = dep?.deployment_id ? String(dep.deployment_id) : "";
              return (
                <option key={`member-dep-${depId || idx}`} value={depId}>
                  {depId || `deployment-${idx + 1}`}
                </option>
              );
            })}
          </select>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || memberAgentsBusy}
            onClick={() => void refreshMemberAgents()}
          >
            {memberAgentsBusy ? "Loading…" : "Refresh agents"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy || memberAgentsBusy}
            onClick={() => void handleAddConnectedAgentsToTeam()}
          >
            Add connected agents
          </button>
          {memberAgentsError ? (
            <span className="text-[11px] text-rose-200">{memberAgentsError}</span>
          ) : null}
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Backend label</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberBackendLabel}
            onChange={(e) => setMemberBackendLabel(e.target.value)}
            placeholder="openrouter-main"
          />
          <FieldLabel>Model</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberModel}
            onChange={(e) => setMemberModel(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Base URL</FieldLabel>
          <input
            className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberBaseUrl}
            onChange={(e) => setMemberBaseUrl(e.target.value)}
            placeholder="https://api.openai.com/v1"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Summary model</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberSummaryModel}
            onChange={(e) => setMemberSummaryModel(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Tools</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberTools}
            onChange={(e) => setMemberTools(e.target.value)}
          >
            <option value="">inherit</option>
            <option value="none">none</option>
            <option value="basic">basic</option>
            <option value="host">host</option>
          </select>
          <FieldLabel>Timeout ms</FieldLabel>
          <input
            className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberTimeoutMs}
            onChange={(e) => setMemberTimeoutMs(e.target.value)}
            placeholder="60000"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy}
            onClick={() => void handleAddMember()}
          >
            Add member
          </button>
        </div>
        {membersError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {membersError}
          </div>
        ) : null}
        {members && members.length > 0 ? (
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <span>Bulk status:</span>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!canQuery || membersBusy}
              onClick={() => void handleSetAllMemberStatus("paused")}
            >
              Pause all
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!canQuery || membersBusy}
              onClick={() => void handleSetAllMemberStatus("active")}
            >
              Resume all
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!canQuery || membersBusy}
              onClick={() => void handleRemovePausedMembers()}
            >
              Remove paused
            </button>
          </div>
        ) : null}
        {members && members.length > 0 ? (
          <div className="grid gap-2">
            {members.map((m, idx) => {
              const mid = String(m?.member_id || "");
              const meta = m?.meta && typeof m.meta === "object" ? (m.meta as Record<string, any>) : null;
              const backendLabel = meta?.backend_label ? String(meta.backend_label) : "";
              const overridesRaw =
                meta?.run_overrides && typeof meta.run_overrides === "object" ? meta.run_overrides : null;
              const overrideBits: string[] = [];
              if (overridesRaw && typeof overridesRaw === "object") {
                const model = (overridesRaw as any).model ? String((overridesRaw as any).model) : "";
                const baseUrl = (overridesRaw as any).base_url ? String((overridesRaw as any).base_url) : "";
                const summaryModel = (overridesRaw as any).summary_model ? String((overridesRaw as any).summary_model) : "";
                const tools = (overridesRaw as any).tools ? String((overridesRaw as any).tools) : "";
                const timeoutMs = (overridesRaw as any).timeout_ms;
                const maxSteps = (overridesRaw as any).max_steps;
                const streamAssistant = (overridesRaw as any).stream_assistant;
                if (model) overrideBits.push(`model ${model}`);
                if (summaryModel) overrideBits.push(`summary ${summaryModel}`);
                if (baseUrl) overrideBits.push(`base ${baseUrl}`);
                if (tools) overrideBits.push(`tools ${tools}`);
                if (Number.isFinite(timeoutMs)) overrideBits.push(`timeout ${timeoutMs}ms`);
                if (Number.isFinite(maxSteps)) overrideBits.push(`max_steps ${maxSteps}`);
                if (typeof streamAssistant === "boolean") {
                  overrideBits.push(`stream ${streamAssistant ? "on" : "off"}`);
                }
              }
              const infoBits: string[] = [];
              if (typeof m?.weight === "number") infoBits.push(`weight ${m.weight}`);
              const caps = Array.isArray(m?.capabilities)
                ? m.capabilities.map((c) => String(c).trim()).filter(Boolean)
                : [];
              if (caps.length > 0) infoBits.push(`caps ${caps.join(",")}`);
              if (backendLabel) infoBits.push(`backend ${backendLabel}`);
              if (overrideBits.length > 0) infoBits.push(`overrides ${overrideBits.join(", ")}`);
              return (
                <div key={`member-${mid}-${idx}`} className="grid gap-2">
                  <div className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                    <div className="text-[11px] text-white/70">
                      <div>
                        <span className="text-white/90">{mid || "member"}</span>
                        {m?.role ? ` · role ${m.role}` : ""}
                        {m?.status ? ` · ${m.status}` : ""}
                        {m?.agent_id ? ` · agent ${m.agent_id}` : ""}
                        {m?.deployment_id ? ` · dep ${m.deployment_id}` : ""}
                        {m?.created_unix_ms ? ` · ${fmtTs(m.created_unix_ms)}` : ""}
                      </div>
                      {infoBits.length > 0 ? (
                        <div className="text-[10px] text-white/50">{infoBits.join(" · ")}</div>
                      ) : null}
                    </div>
                    <div className="flex items-center gap-2">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => void handleToggleMemberStatus(m)}
                      >
                        {String(m?.status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => handleEditMember(m)}
                      >
                        Edit
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => void handleDeleteMember(mid)}
                      >
                        Remove
                      </button>
                    </div>
                  </div>
                  {memberEditId === mid ? (
                    <div
                      data-testid="team-member-edit"
                      className="rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70"
                    >
                      <div className="flex flex-wrap items-center gap-2">
                        <FieldLabel>Role</FieldLabel>
                        <input
                          className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-role"
                          value={memberEditRole}
                          onChange={(e) => setMemberEditRole(e.target.value)}
                        />
                        <FieldLabel>Status</FieldLabel>
                        <select
                          className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-status"
                          value={memberEditStatus}
                          onChange={(e) => setMemberEditStatus(e.target.value)}
                        >
                          <option value="active">active</option>
                          <option value="paused">paused</option>
                          <option value="disabled">disabled</option>
                        </select>
                        <FieldLabel>Weight</FieldLabel>
                        <input
                          className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-weight"
                          value={memberEditWeight}
                          onChange={(e) => setMemberEditWeight(e.target.value)}
                          placeholder="1"
                        />
                      </div>
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <FieldLabel>Agent ID</FieldLabel>
                        <input
                          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-agent-id"
                          value={memberEditAgentId}
                          onChange={(e) => setMemberEditAgentId(e.target.value)}
                          placeholder="agent1"
                        />
                        <FieldLabel>Agent pick</FieldLabel>
                        <select
                          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-agent-pick"
                          value={memberEditAgentId}
                          onChange={(e) => setMemberEditAgentId(e.target.value)}
                        >
                          <option value="">(select agent)</option>
                          {memberAgentOptions.map((agent) => {
                            const aid = String(agent?.agent_id || "");
                            if (!aid) return null;
                            const suffix = agent?.connected ? " · connected" : "";
                            return (
                              <option key={`member-edit-agent-${aid}`} value={aid}>
                                {aid}
                                {suffix}
                              </option>
                            );
                          })}
                        </select>
                        <FieldLabel>Deployment</FieldLabel>
                        <select
                          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-deployment"
                          value={memberEditDeploymentId}
                          onChange={(e) => setMemberEditDeploymentId(e.target.value)}
                          disabled={memberEditAgentDeployments.length === 0}
                        >
                          <option value="">(default)</option>
                          {memberEditAgentDeployments.map((dep, idx) => {
                            const depId = dep?.deployment_id ? String(dep.deployment_id) : "";
                            return (
                              <option key={`member-edit-dep-${depId || idx}`} value={depId}>
                                {depId || `deployment-${idx + 1}`}
                              </option>
                            );
                          })}
                        </select>
                      </div>
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <FieldLabel>Capabilities</FieldLabel>
                        <input
                          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-caps"
                          value={memberEditCapabilities}
                          onChange={(e) => setMemberEditCapabilities(e.target.value)}
                          placeholder="vision,audio"
                        />
                      </div>
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <FieldLabel>Backend label</FieldLabel>
                        <input
                          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-backend"
                          value={memberEditBackendLabel}
                          onChange={(e) => setMemberEditBackendLabel(e.target.value)}
                          placeholder="openrouter-main"
                        />
                        <FieldLabel>Model</FieldLabel>
                        <input
                          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-model"
                          value={memberEditModel}
                          onChange={(e) => setMemberEditModel(e.target.value)}
                          placeholder="gpt-4.1-mini"
                        />
                        <FieldLabel>Tools</FieldLabel>
                        <input
                          className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-tools"
                          value={memberEditTools}
                          onChange={(e) => setMemberEditTools(e.target.value)}
                          placeholder="basic"
                        />
                      </div>
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <FieldLabel>Base URL</FieldLabel>
                        <input
                          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-base-url"
                          value={memberEditBaseUrl}
                          onChange={(e) => setMemberEditBaseUrl(e.target.value)}
                          placeholder="https://api.openai.com/v1"
                        />
                        <FieldLabel>Summary model</FieldLabel>
                        <input
                          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-summary-model"
                          value={memberEditSummaryModel}
                          onChange={(e) => setMemberEditSummaryModel(e.target.value)}
                          placeholder="gpt-4.1-mini"
                        />
                        <FieldLabel>Timeout ms</FieldLabel>
                        <input
                          className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                          data-testid="team-member-edit-timeout"
                          value={memberEditTimeoutMs}
                          onChange={(e) => setMemberEditTimeoutMs(e.target.value)}
                          placeholder="60000"
                        />
                      </div>
                      <div className="mt-2 grid gap-1">
                        <FieldLabel>Meta JSON</FieldLabel>
                        <textarea
                          className="min-h-[72px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                          data-testid="team-member-edit-meta"
                          value={memberEditMetaJson}
                          onChange={(e) => setMemberEditMetaJson(e.target.value)}
                          placeholder='{"backend_label":"openrouter-main"}'
                        />
                      </div>
                      <div className="mt-2 flex flex-wrap items-center gap-2">
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                          type="button"
                          disabled={!canQuery || memberEditBusy}
                          onClick={() => void handleSaveMemberEdit()}
                        >
                          {memberEditBusy ? "Saving…" : "Save"}
                        </button>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                          type="button"
                          onClick={() => handleCancelMemberEdit()}
                        >
                          Cancel
                        </button>
                        {memberEditError ? (
                          <span className="text-[11px] text-rose-200">{memberEditError}</span>
                        ) : null}
                      </div>
                    </div>
                  ) : null}
                </div>
              );
            })}
          </div>
        ) : null}
      </SectionCard>
      ) : null}

      {teamTab === "advanced" ? (
        <>
          <SectionCard title="Quorum rules" description="Approvals and quorum settings.">
        <div className="flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Quorum rules</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || rulesBusy}
            onClick={() => void refreshRules(teamIdTrimmed)}
          >
            {rulesBusy ? "Loading…" : "Refresh"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Action</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleAction}
            onChange={(e) => setRuleAction(e.target.value)}
          />
          <FieldLabel>Min approvals</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleMinApprovals}
            onChange={(e) => setRuleMinApprovals(e.target.value)}
          />
          <FieldLabel>Mode</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleMode}
            onChange={(e) => setRuleMode(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || rulesBusy}
            onClick={() => void handleAddRule()}
          >
            Add rule
          </button>
        </div>
        {rulesError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {rulesError}
          </div>
        ) : null}
        {rules && rules.length > 0 ? (
          <div className="grid gap-2">
            {rules.map((r, idx) => {
              const rid = String(r?.rule_id || "");
              return (
                <div
                  key={`rule-${rid}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <span className="text-white/90">{r?.action || "team_run"}</span>
                    {r?.min_approvals ? ` · min ${r.min_approvals}` : ""}
                    {r?.quorum_mode ? ` · ${r.quorum_mode}` : ""}
                    {r?.created_unix_ms ? ` · ${fmtTs(r.created_unix_ms)}` : ""}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => void handleDeleteRule(rid)}
                  >
                    Remove
                  </button>
                </div>
              );
            })}
          </div>
        ) : null}
          </SectionCard>

          <SectionCard title="Team event replay" description="Replay recent events for auditing." defaultOpen={false}>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || teamReplayBusy}
            onClick={() => void loadTeamReplay()}
          >
            {teamReplayBusy ? "Replaying…" : "Replay"}
          </button>
          {teamReplayNote ? <span className="text-emerald-200">{teamReplayNote}</span> : null}
          {teamReplayError ? <span className="text-rose-200">{teamReplayError}</span> : null}
        </div>
          </SectionCard>

          <SectionCard title="Orchestrator runs" description="Low-level orchestrator controls." defaultOpen={false}>
        <BrokerOrchestratorRunPanel
          base={props.base}
          auth={props.auth}
          canQuery={canQuery}
          teamId={teamIdTrimmed}
          teamMeta={teamDetails?.meta && typeof teamDetails.meta === "object" ? (teamDetails.meta as Record<string, any>) : null}
          events={mergedOrchestratorEvents}
        />
          </SectionCard>

          <SectionCard title="Guidance" description="Send guidance and review receipts." defaultOpen={false}>
        <BrokerTeamGuidancePanel
          base={props.base}
          auth={props.auth}
          canQuery={canQuery}
          teamId={teamIdTrimmed}
          events={mergedGuidanceEvents}
        />
          </SectionCard>

          <SectionCard title="Spawn requests" description="Monitor orchestrator spawn requests." defaultOpen={false}>
        <BrokerOrchestratorSpawnPanel
          base={props.base}
          auth={props.auth}
          canQuery={canQuery}
          teamId={teamIdTrimmed}
          events={mergedOrchestratorEvents}
        />
          </SectionCard>
        </>
      ) : null}

    </section>
  );
}
