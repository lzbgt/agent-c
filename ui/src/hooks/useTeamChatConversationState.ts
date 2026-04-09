import React from "react";
import { useQuery } from "@tanstack/react-query";
import {
  apiBrokerProxyJson,
  apiBrokerTeamGuidanceList,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunList,
  type ApiAuth,
  type BrokerGuidanceEvent,
  type BrokerTeamRunStatusResp,
  type BrokerTeamRunSummary,
} from "../api";
import useLocalStorageState from "./useLocalStorageState";
import type {
  TeamActivity,
  TeamConversationCache,
  TeamConversationItem,
  TeamConversationMemberMeta,
  TeamConversationMessage,
  TeamConversationMeta,
  TeamConversationPayload,
  TeamRecentActivity,
  TeamQueuedAction,
  TeamRunSessionSnapshot,
} from "./teamChatOrchestrationTypes";

type TeamChatConversationStateArgs = {
  authKey: string;
  brokerAgentId: string;
  brokerBase: string;
  brokerChatAvailable: boolean;
  daemonAuth: ApiAuth;
  selectedTeamId: string;
};

type TeamChatQueryResult = {
  status: BrokerTeamRunStatusResp | null;
  items: TeamConversationItem[];
  warnings: string[];
};

type TeamStatusMember = {
  member_id?: string;
  role?: string;
  agent_id?: string;
  deployment_id?: string;
};

type TeamStatusMemberJob = {
  member_id?: string;
  agent_id?: string;
  deployment_id?: string;
};

const isUnknownRecord = (value: unknown): value is Record<string, unknown> =>
  Boolean(value) && typeof value === "object" && !Array.isArray(value);

const asTrimmedString = (value: unknown): string | undefined => {
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  return trimmed || undefined;
};

const asFiniteNumber = (value: unknown): number | undefined => {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim()) {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return undefined;
};

const toConversationPayload = (value: unknown): TeamConversationPayload | undefined => {
  if (value === null) return null;
  return isUnknownRecord(value) ? value : undefined;
};

const toConversationMessage = (value: unknown): TeamConversationMessage | null => {
  if (!isUnknownRecord(value)) return null;
  const createdUnixMs = asFiniteNumber(value.created_unix_ms);
  if (!createdUnixMs) return null;
  return {
    role: asTrimmedString(value.role) || "message",
    content: typeof value.content === "string" ? value.content : "",
    created_unix_ms: createdUnixMs,
    content_truncated: value.content_truncated === true ? true : undefined,
    mm_json: typeof value.mm_json === "string" ? value.mm_json : undefined,
    mm_bytes: asFiniteNumber(value.mm_bytes),
  };
};

const toConversationMeta = (value: unknown): TeamConversationMeta => {
  if (!isUnknownRecord(value)) return {};
  return {
    member_id: asTrimmedString(value.member_id),
    role: asTrimmedString(value.role),
    agent_id: asTrimmedString(value.agent_id),
    deployment_id: asTrimmedString(value.deployment_id),
    session_id: asTrimmedString(value.session_id),
    run_id: asTrimmedString(value.run_id),
    guidance_id: asTrimmedString(value.guidance_id),
    kind: asTrimmedString(value.kind),
    priority: asTrimmedString(value.priority),
    status: asTrimmedString(value.status),
    payload: toConversationPayload(value.payload),
  };
};

const normalizeCachedTeamConversationItem = (value: unknown): TeamConversationItem | null => {
  if (!isUnknownRecord(value)) return null;
  const ts = asFiniteNumber(value.ts);
  const message = toConversationMessage(value.message);
  if (!ts || !message) return null;
  const kindRaw = asTrimmedString(value.kind) || "message";
  const kind = kindRaw === "guidance" || kindRaw === "goal" || kindRaw === "prompt" ? kindRaw : "message";
  return {
    kind,
    ts,
    message,
    meta: toConversationMeta(value.meta),
  };
};

const normalizeCachedTeamConversationItems = (value: unknown): TeamConversationItem[] =>
  Array.isArray(value)
    ? value
        .map((item) => normalizeCachedTeamConversationItem(item))
        .filter((item): item is TeamConversationItem => item !== null)
    : [];

const sortRunsByCreatedDesc = (runs: BrokerTeamRunSummary[]): BrokerTeamRunSummary[] =>
  runs.slice().sort((a, b) => (Number(b?.created_unix_ms || 0) || 0) - (Number(a?.created_unix_ms || 0) || 0));

const buildMemberMeta = (
  members: TeamStatusMember[],
  memberJobs: TeamStatusMemberJob[],
): Record<string, TeamConversationMemberMeta> => {
  const memberMeta: Record<string, TeamConversationMemberMeta> = {};
  for (const member of members) {
    const memberId = String(member?.member_id || "").trim();
    if (!memberId) continue;
    memberMeta[memberId] = {
      role: asTrimmedString(member?.role),
      agent_id: asTrimmedString(member?.agent_id),
      deployment_id: asTrimmedString(member?.deployment_id),
    };
  }
  for (const job of memberJobs) {
    const memberId = String(job?.member_id || "").trim();
    if (!memberId) continue;
    const previous = memberMeta[memberId] || {};
    memberMeta[memberId] = {
      role: previous.role,
      agent_id: asTrimmedString(job?.agent_id) || previous.agent_id,
      deployment_id: asTrimmedString(job?.deployment_id) || previous.deployment_id,
    };
  }
  return memberMeta;
};

const extractProxyDbMessages = (value: unknown): TeamConversationMessage[] => {
  if (!isUnknownRecord(value) || value.ok !== true || !Array.isArray(value.messages)) return [];
  return value.messages
    .map((message) => toConversationMessage(message))
    .filter((message): message is TeamConversationMessage => message !== null);
};

const toPromptConversationItem = (entry: TeamActivity): TeamConversationItem | null => {
  const ts = typeof entry.ts === "number" ? entry.ts : 0;
  const promptValue = String(entry.prompt || "").trim();
  if (!ts || !promptValue) return null;
  const runId = String(entry.run_id || "").trim() || undefined;
  const payload = entry.payload === undefined ? undefined : entry.payload;
  if (entry.kind === "guidance") {
    return {
      kind: "guidance",
      ts,
      message: { role: "guidance", content: promptValue },
      meta: { run_id: runId, payload },
    };
  }
  if (entry.kind === "goal") {
    return {
      kind: "goal",
      ts,
      message: { role: "goal", content: promptValue },
      meta: { run_id: runId, payload },
    };
  }
  return {
    kind: "prompt",
    ts,
    message: { role: "user", content: promptValue },
    meta: { run_id: runId },
  };
};

export function useTeamChatConversationState(args: TeamChatConversationStateArgs) {
  const { authKey, brokerAgentId, brokerBase, brokerChatAvailable, daemonAuth, selectedTeamId } = args;

  const selectedTeamIdTrimmed = String(selectedTeamId || "").trim();
  const brokerBaseTrimmed = String(brokerBase || "").trim();

  const [teamPromptHistory, setTeamPromptHistory] = useLocalStorageState<Record<string, TeamActivity[]>>(
    "agentui.teamPrompts",
    {},
  );
  const [teamRunSessionsByKey, setTeamRunSessionsByKey] = useLocalStorageState<Record<string, TeamRunSessionSnapshot>>(
    "agentui.teamRunSessions",
    {},
  );

  const teamQueueKey = React.useMemo(() => {
    const base = brokerBaseTrimmed || "default";
    const tid = selectedTeamIdTrimmed || "none";
    return `agentui.teamQueue:${base}::${tid}`;
  }, [brokerBaseTrimmed, selectedTeamIdTrimmed]);
  const [teamQueue, setTeamQueue] = useLocalStorageState<TeamQueuedAction[]>(teamQueueKey, []);
  const teamQueueEntries = Array.isArray(teamQueue) ? teamQueue : [];
  const teamQueueCount = teamQueueEntries.length;

  const teamConversationCacheKey = React.useMemo(() => {
    const base = brokerBaseTrimmed || "default";
    const tid = selectedTeamIdTrimmed || "none";
    return `agentui.teamChatCache:${base}::${tid}`;
  }, [brokerBaseTrimmed, selectedTeamIdTrimmed]);
  const [teamConversationCache, setTeamConversationCache] = useLocalStorageState<TeamConversationCache>(
    teamConversationCacheKey,
    { items: [], updated_ms: 0 },
  );
  const cachedTeamConversationItems = React.useMemo(
    () => normalizeCachedTeamConversationItems(teamConversationCache?.items),
    [teamConversationCache?.items],
  );

  const teamRunList = useQuery({
    queryKey: ["broker_team_runs", brokerBaseTrimmed, authKey, selectedTeamIdTrimmed],
    queryFn: () => apiBrokerTeamRunList(brokerBaseTrimmed, selectedTeamIdTrimmed, daemonAuth, { limit: 6, offset: 0 }),
    enabled: brokerChatAvailable && !!brokerBaseTrimmed && selectedTeamIdTrimmed.length > 0,
    refetchInterval: brokerChatAvailable ? 6000 : false,
    retry: 1,
  });

  const recentTeamRuns = React.useMemo(() => {
    const runs = teamRunList.data?.ok && Array.isArray(teamRunList.data?.runs) ? teamRunList.data.runs : [];
    return sortRunsByCreatedDesc(runs);
  }, [teamRunList.data]);

  const latestTeamRunId = React.useMemo(
    () => String(recentTeamRuns[0]?.team_run_id || "").trim(),
    [recentTeamRuns],
  );

  const latestTeamRunCreatedMs = React.useMemo(
    () => Number(recentTeamRuns[0]?.created_unix_ms || 0) || 0,
    [recentTeamRuns],
  );

  const teamRunSessionKey = React.useMemo(() => {
    if (!brokerBaseTrimmed || !selectedTeamIdTrimmed || !latestTeamRunId) return "";
    return `${brokerBaseTrimmed}::${selectedTeamIdTrimmed}::${latestTeamRunId}`;
  }, [brokerBaseTrimmed, latestTeamRunId, selectedTeamIdTrimmed]);

  const teamChat = useQuery<TeamChatQueryResult>({
    queryKey: ["broker_team_chat", brokerBaseTrimmed, authKey, selectedTeamIdTrimmed, latestTeamRunId],
    queryFn: async () => {
      if (!latestTeamRunId) {
        return { status: null, items: [], warnings: [] };
      }
      const storedSnapshot =
        teamRunSessionKey && teamRunSessionsByKey[teamRunSessionKey] ? teamRunSessionsByKey[teamRunSessionKey] : undefined;
      const status = await apiBrokerTeamRunGet(brokerBaseTrimmed, selectedTeamIdTrimmed, latestTeamRunId, daemonAuth);
      if (!status.ok) {
        return { status, items: [], warnings: [status.error || status.err || status.code || "team run failed"] };
      }

      const rawMemberSessions =
        status.member_sessions && typeof status.member_sessions === "object" ? status.member_sessions : {};
      const members = Array.isArray(status.members) ? status.members : [];
      const memberJobs = Array.isArray(status.member_jobs) ? status.member_jobs : [];
      const memberMeta = buildMemberMeta(members, memberJobs);
      const hasStatusMembers = members.length > 0 || memberJobs.length > 0;
      const hasStatusSessions = Object.keys(rawMemberSessions).length > 0;

      const memberSessions = hasStatusSessions ? (rawMemberSessions as Record<string, string>) : storedSnapshot?.sessions || {};
      const mergedMemberMeta = hasStatusMembers ? memberMeta : storedSnapshot?.members || {};

      if (teamRunSessionKey && (hasStatusSessions || hasStatusMembers)) {
        setTeamRunSessionsByKey((prev) => ({
          ...prev,
          [teamRunSessionKey]: {
            sessions: hasStatusSessions ? (rawMemberSessions as Record<string, string>) : storedSnapshot?.sessions || {},
            members: hasStatusMembers ? memberMeta : storedSnapshot?.members || {},
            updated_unix_ms: Date.now(),
          },
        }));
      }

      const warnings: string[] = [];
      const items: TeamConversationItem[] = [];
      await Promise.all(
        Object.entries(memberSessions as Record<string, string>).map(async ([memberId, sessionId]) => {
          const sid = String(sessionId || "").trim();
          if (!sid) return;
          const meta = mergedMemberMeta[String(memberId || "").trim()] || {};
          const agentId = meta.agent_id || brokerAgentId;
          if (!agentId) {
            warnings.push(`missing agent_id for member ${memberId}`);
            return;
          }
          const depId = meta.deployment_id;
          const path = `/api/v1/db/messages?session_id=${encodeURIComponent(sid)}&limit=80&offset=0&max_content_bytes=65536&max_mm_bytes=0`;
          try {
            const resp = await apiBrokerProxyJson(brokerBaseTrimmed, agentId, path, "GET", undefined, daemonAuth, depId);
            const messages = extractProxyDbMessages(resp?.data);
            for (const message of messages) {
              const ts = message.created_unix_ms || 0;
              items.push({
                kind: "message",
                ts,
                message,
                meta: {
                  member_id: memberId,
                  role: meta.role,
                  agent_id: agentId,
                  session_id: sid,
                  run_id: latestTeamRunId || undefined,
                },
              });
            }
          } catch (error) {
            warnings.push(`failed to load messages for ${memberId}: ${String(error)}`);
          }
        }),
      );

      items.sort((a, b) => a.ts - b.ts);
      return { status, items, warnings };
    },
    enabled: brokerChatAvailable && !!brokerBaseTrimmed && !!latestTeamRunId,
    refetchInterval: brokerChatAvailable ? 5000 : false,
    retry: 1,
  });

  const teamGuidance = useQuery({
    queryKey: ["broker_team_guidance", brokerBaseTrimmed, authKey, selectedTeamIdTrimmed, latestTeamRunId],
    queryFn: () =>
      apiBrokerTeamGuidanceList(
        brokerBaseTrimmed,
        selectedTeamIdTrimmed,
        { teamRunId: latestTeamRunId, limit: 50, offset: 0 },
        daemonAuth,
      ),
    enabled: brokerChatAvailable && !!brokerBaseTrimmed && selectedTeamIdTrimmed.length > 0,
    refetchInterval: brokerChatAvailable ? 6000 : false,
    retry: 1,
  });

  const teamQueueNeedsRun =
    teamQueueCount > 0 && !latestTeamRunId && teamQueueEntries.some((entry) => entry.action !== "run");

  const teamGuidanceRows = React.useMemo<BrokerGuidanceEvent[]>(
    () => (teamGuidance.data?.ok && Array.isArray(teamGuidance.data?.guidance) ? teamGuidance.data.guidance : []),
    [teamGuidance.data],
  );

  const teamPromptEntries = React.useMemo<TeamActivity[]>(
    () => (Array.isArray(teamPromptHistory[selectedTeamIdTrimmed]) ? teamPromptHistory[selectedTeamIdTrimmed] : []),
    [selectedTeamIdTrimmed, teamPromptHistory],
  );

  const teamConversationLiveCount = React.useMemo(() => {
    return (teamChat.data?.items || []).length + teamGuidanceRows.length + teamPromptEntries.length;
  }, [teamChat.data?.items, teamGuidanceRows.length, teamPromptEntries.length]);

  const teamConversationItems = React.useMemo(() => {
    const out: TeamConversationItem[] = [...(teamChat.data?.items || [])];

    for (const guidance of teamGuidanceRows) {
      const ts = typeof guidance?.created_unix_ms === "number" ? guidance.created_unix_ms : 0;
      const message = typeof guidance?.message === "string" ? guidance.message : "";
      if (!ts || !message) continue;
      out.push({
        kind: "guidance",
        ts,
        message: { role: "guidance", content: message },
        meta: {
          guidance_id: asTrimmedString(guidance?.guidance_id),
          kind: asTrimmedString(guidance?.kind),
          priority: asTrimmedString(guidance?.priority),
          status: asTrimmedString(guidance?.status),
          run_id: asTrimmedString(guidance?.team_run_id),
          payload: toConversationPayload(guidance?.payload),
        },
      });
    }

    for (const promptEntry of teamPromptEntries) {
      const item = toPromptConversationItem(promptEntry);
      if (item) out.push(item);
    }

    out.sort((a, b) => a.ts - b.ts);
    const deduped: TeamConversationItem[] = [];
    const seenPromptKeys = new Set<string>();
    for (const item of out) {
      const role = String(item.message.role || "");
      const content = String(item.message.content || "");
      if (role === "user" && content) {
        const key = content.slice(0, 200);
        if (seenPromptKeys.has(key)) continue;
        seenPromptKeys.add(key);
      }
      deduped.push(item);
    }
    return deduped.length > 0 ? deduped : cachedTeamConversationItems;
  }, [cachedTeamConversationItems, teamChat.data?.items, teamGuidanceRows, teamPromptEntries]);

  const teamConversationUsingCache = teamConversationLiveCount === 0 && cachedTeamConversationItems.length > 0;
  const teamConversationCacheUpdatedMs =
    typeof teamConversationCache?.updated_ms === "number" ? teamConversationCache.updated_ms : 0;

  const teamRecentActivity = React.useMemo(() => {
    const items = teamConversationItems;
    const out: TeamRecentActivity[] = [];
    for (let i = items.length - 1; i >= 0 && out.length < 3; i -= 1) {
      const item = items[i];
      const ts = typeof item.ts === "number" ? item.ts : 0;
      const role = String(item.message.role || "").trim() || "message";
      const meta = item.meta;
      const agentLabel = typeof meta.agent_id === "string" ? meta.agent_id : "";
      const label =
        role === "guidance"
          ? "guidance"
          : role === "goal"
            ? "goal"
            : role === "user"
              ? "you"
              : agentLabel || role;
      const content = String(item.message.content || "").trim();
      if (!content) continue;
      const preview = content.length > 140 ? `${content.slice(0, 140)}...` : content;
      out.push({ key: `${ts}-${i}`, label, preview, ts });
    }
    return out;
  }, [teamConversationItems]);

  React.useEffect(() => {
    if (teamConversationLiveCount === 0) return;
    if (!Array.isArray(teamConversationItems) || teamConversationItems.length === 0) return;
    setTeamConversationCache({
      items: teamConversationItems.slice(-200),
      updated_ms: Date.now(),
    });
  }, [setTeamConversationCache, teamConversationItems, teamConversationLiveCount]);

  const addTeamActivity = React.useCallback(
    (entry: TeamActivity) => {
      const teamId = selectedTeamIdTrimmed;
      if (!teamId) return;
      setTeamPromptHistory((prev) => {
        const current = Array.isArray(prev[teamId]) ? prev[teamId] : [];
        return { ...prev, [teamId]: [...current, entry] };
      });
    },
    [selectedTeamIdTrimmed, setTeamPromptHistory],
  );

  const teamStatus =
    typeof teamChat.data?.status?.status === "string"
      ? teamChat.data?.status?.status
      : typeof teamChat.data?.status?.code === "string"
        ? teamChat.data?.status?.code
        : "";

  const teamConversationWarnings = [
    ...(teamChat.data?.warnings || []),
    ...(teamGuidance.isError ? [String(teamGuidance.error)] : []),
    ...(teamGuidance.data?.ok === false
      ? [teamGuidance.data?.error || teamGuidance.data?.err || teamGuidance.data?.code || "guidance error"]
      : []),
  ].filter(Boolean);

  return {
    addTeamActivity,
    brokerBaseTrimmed,
    latestTeamRunCreatedMs,
    latestTeamRunId,
    selectedTeamIdTrimmed,
    setTeamQueue,
    teamConversationCacheUpdatedMs,
    teamConversationItems,
    teamConversationUsingCache,
    teamConversationWarnings,
    teamGuidance,
    teamQueue: teamQueueEntries,
    teamQueueCount,
    teamQueueNeedsRun,
    teamRecentActivity,
    teamRunList,
    teamStatus,
  };
}
