import React from "react";
import { useQuery } from "@tanstack/react-query";
import {
  apiBrokerProxyJson,
  apiBrokerTeamGuidanceList,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunList,
  type ApiAuth,
} from "../api";
import useLocalStorageState from "./useLocalStorageState";
import type {
  TeamActivity,
  TeamConversationCache,
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
  const cachedTeamConversationItems = Array.isArray(teamConversationCache?.items) ? teamConversationCache.items : [];

  const teamRunList = useQuery({
    queryKey: ["broker_team_runs", brokerBaseTrimmed, authKey, selectedTeamIdTrimmed],
    queryFn: () => apiBrokerTeamRunList(brokerBaseTrimmed, selectedTeamIdTrimmed, daemonAuth, { limit: 6, offset: 0 }),
    enabled: brokerChatAvailable && !!brokerBaseTrimmed && selectedTeamIdTrimmed.length > 0,
    refetchInterval: brokerChatAvailable ? 6000 : false,
    retry: 1,
  });

  const latestTeamRunId = React.useMemo(() => {
    const runs = teamRunList.data?.ok && Array.isArray(teamRunList.data?.runs) ? (teamRunList.data.runs as any[]) : [];
    if (runs.length === 0) return "";
    const sorted = runs
      .slice()
      .sort((a, b) => (Number(b?.created_unix_ms || 0) || 0) - (Number(a?.created_unix_ms || 0) || 0));
    return String(sorted[0]?.team_run_id || "").trim();
  }, [teamRunList.data]);

  const latestTeamRunCreatedMs = React.useMemo(() => {
    const runs = teamRunList.data?.ok && Array.isArray(teamRunList.data?.runs) ? (teamRunList.data.runs as any[]) : [];
    if (runs.length === 0) return 0;
    const sorted = runs
      .slice()
      .sort((a, b) => (Number(b?.created_unix_ms || 0) || 0) - (Number(a?.created_unix_ms || 0) || 0));
    return Number(sorted[0]?.created_unix_ms || 0) || 0;
  }, [teamRunList.data]);

  const teamRunSessionKey = React.useMemo(() => {
    if (!brokerBaseTrimmed || !selectedTeamIdTrimmed || !latestTeamRunId) return "";
    return `${brokerBaseTrimmed}::${selectedTeamIdTrimmed}::${latestTeamRunId}`;
  }, [brokerBaseTrimmed, latestTeamRunId, selectedTeamIdTrimmed]);

  const teamChat = useQuery({
    queryKey: ["broker_team_chat", brokerBaseTrimmed, authKey, selectedTeamIdTrimmed, latestTeamRunId],
    queryFn: async () => {
      if (!latestTeamRunId) {
        return { status: null as any, items: [] as any[], warnings: [] as string[] };
      }
      const storedSnapshot =
        teamRunSessionKey && teamRunSessionsByKey[teamRunSessionKey] ? teamRunSessionsByKey[teamRunSessionKey] : undefined;
      const status = await apiBrokerTeamRunGet(brokerBaseTrimmed, selectedTeamIdTrimmed, latestTeamRunId, daemonAuth);
      if (!status.ok) {
        return { status, items: [] as any[], warnings: [status.error || status.err || status.code || "team run failed"] };
      }

      const rawMemberSessions =
        status.member_sessions && typeof status.member_sessions === "object" ? status.member_sessions : {};
      const members = Array.isArray(status.members) ? status.members : [];
      const memberJobs = Array.isArray(status.member_jobs) ? status.member_jobs : [];
      const memberMeta: Record<string, { role?: string; agent_id?: string; deployment_id?: string }> = {};
      const hasStatusMembers = members.length > 0 || memberJobs.length > 0;
      const hasStatusSessions = Object.keys(rawMemberSessions).length > 0;

      for (const member of members) {
        const id = String((member as any)?.member_id || "").trim();
        if (!id) continue;
        memberMeta[id] = {
          role: String((member as any)?.role || "").trim() || undefined,
          agent_id: String((member as any)?.agent_id || "").trim() || undefined,
          deployment_id: String((member as any)?.deployment_id || "").trim() || undefined,
        };
      }
      for (const job of memberJobs) {
        const id = String((job as any)?.member_id || "").trim();
        if (!id) continue;
        const prev = memberMeta[id] || {};
        memberMeta[id] = {
          role: prev.role,
          agent_id: String((job as any)?.agent_id || prev.agent_id || "").trim() || undefined,
          deployment_id: String((job as any)?.deployment_id || prev.deployment_id || "").trim() || undefined,
        };
      }

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
      const items: any[] = [];
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
            const data = resp?.data;
            const msgs = data?.ok && Array.isArray(data?.messages) ? (data.messages as any[]) : [];
            for (const message of msgs) {
              const ts = typeof message?.created_unix_ms === "number" ? message.created_unix_ms : 0;
              if (!ts) continue;
              items.push({
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

  const teamConversationLiveCount = React.useMemo(() => {
    const items = Array.isArray(teamChat.data?.items) ? teamChat.data.items : [];
    const guidance = Array.isArray(teamGuidance.data?.guidance) ? teamGuidance.data.guidance : [];
    const prompts = teamPromptHistory[selectedTeamIdTrimmed] || [];
    return items.length + guidance.length + prompts.length;
  }, [selectedTeamIdTrimmed, teamChat.data, teamGuidance.data, teamPromptHistory]);

  const teamConversationItems = React.useMemo(() => {
    const out: any[] = [];
    const items = Array.isArray(teamChat.data?.items) ? (teamChat.data?.items as any[]) : [];
    for (const item of items) {
      const message = item?.message;
      if (!message || typeof message !== "object") continue;
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      if (!ts) continue;
      out.push({
        kind: "message",
        ts,
        message,
        meta: item?.meta ?? {},
      });
    }

    const guidanceRows =
      teamGuidance.data?.ok && Array.isArray(teamGuidance.data?.guidance) ? (teamGuidance.data.guidance as any[]) : [];
    for (const guidance of guidanceRows) {
      const ts = typeof guidance?.created_unix_ms === "number" ? guidance.created_unix_ms : 0;
      const message = typeof guidance?.message === "string" ? guidance.message : "";
      if (!ts || !message) continue;
      out.push({
        kind: "guidance",
        ts,
        message: { role: "guidance", content: message },
        meta: {
          guidance_id: guidance?.guidance_id,
          kind: guidance?.kind,
          priority: guidance?.priority,
          status: guidance?.status,
          run_id: guidance?.team_run_id,
          payload: guidance?.payload,
        },
      });
    }

    const prompts = teamPromptHistory[selectedTeamIdTrimmed] || [];
    for (const promptEntry of prompts) {
      if (!promptEntry || typeof promptEntry !== "object") continue;
      const ts = typeof promptEntry.ts === "number" ? promptEntry.ts : 0;
      const promptValue = String((promptEntry as any).prompt || "").trim();
      if (!ts || !promptValue) continue;
      const kind = typeof (promptEntry as any).kind === "string" ? (promptEntry as any).kind : "prompt";
      if (kind === "guidance") {
        out.push({
          kind: "guidance",
          ts,
          message: { role: "guidance", content: promptValue },
          meta: { run_id: (promptEntry as any).run_id || undefined, payload: (promptEntry as any).payload },
        });
      } else if (kind === "goal") {
        out.push({
          kind: "goal",
          ts,
          message: { role: "goal", content: promptValue },
          meta: { run_id: (promptEntry as any).run_id || undefined, payload: (promptEntry as any).payload },
        });
      } else {
        out.push({
          kind: "prompt",
          ts,
          message: { role: "user", content: promptValue },
          meta: { run_id: (promptEntry as any).run_id || undefined },
        });
      }
    }

    out.sort((a, b) => a.ts - b.ts);
    const deduped: any[] = [];
    const seenPromptKeys = new Set<string>();
    for (const item of out) {
      const role = String(item?.message?.role || "");
      const content = String(item?.message?.content || "");
      if (role === "user" && content) {
        const key = content.slice(0, 200);
        if (seenPromptKeys.has(key)) continue;
        seenPromptKeys.add(key);
      }
      deduped.push(item);
    }
    return deduped.length > 0 ? deduped : cachedTeamConversationItems;
  }, [cachedTeamConversationItems, selectedTeamIdTrimmed, teamChat.data, teamGuidance.data, teamPromptHistory]);

  const teamConversationUsingCache = teamConversationLiveCount === 0 && cachedTeamConversationItems.length > 0;
  const teamConversationCacheUpdatedMs =
    typeof teamConversationCache?.updated_ms === "number" ? teamConversationCache.updated_ms : 0;

  const teamRecentActivity = React.useMemo(() => {
    const items = Array.isArray(teamConversationItems) ? teamConversationItems : [];
    const out: Array<{ key: string; label: string; preview: string; ts: number }> = [];
    for (let i = items.length - 1; i >= 0 && out.length < 3; i -= 1) {
      const item = items[i];
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      const role = String(item?.message?.role || "").trim() || "message";
      const meta = item?.meta ?? {};
      const agentLabel = typeof meta?.agent_id === "string" ? meta.agent_id : "";
      const label =
        role === "guidance"
          ? "guidance"
          : role === "goal"
            ? "goal"
            : role === "user"
              ? "you"
              : agentLabel || role;
      const content = String(item?.message?.content || "").trim();
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
    ...(Array.isArray(teamChat.data?.warnings) ? teamChat.data?.warnings : []),
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
