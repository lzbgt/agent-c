import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiBrokerProxyJson,
  apiBrokerTeamGuidanceCreate,
  apiBrokerTeamGuidanceList,
  apiBrokerTeamRunCreate,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunGoalUpdate,
  apiBrokerTeamRunList,
  type ApiAuth,
} from "../api";
import type { Attachment } from "../components/PromptBar";
import useLocalStorageState from "./useLocalStorageState";

export type TeamActionKind = "run" | "guidance" | "goal";

export type TeamQueuedAction = {
  prompt: string;
  attachments: Attachment[];
  queued_unix_ms: number;
  action: TeamActionKind;
};

type TeamActivity = {
  ts: number;
  prompt: string;
  run_id?: string;
  kind?: "prompt" | "guidance" | "goal";
  payload?: any;
};

type TeamRunSessionSnapshot = {
  sessions: Record<string, string>;
  members: Record<string, { role?: string; agent_id?: string; deployment_id?: string }>;
  updated_unix_ms: number;
};

type TeamConversationCache = {
  items: any[];
  updated_ms: number;
};

export type TeamChatOrchestrationArgs = {
  authKey: string;
  brokerAgentId: string;
  brokerBase: string;
  brokerChatAvailable: boolean;
  connectionMode: string;
  daemonAuth: ApiAuth;
  selectedTeamId: string;
  setAdvancedPage: React.Dispatch<React.SetStateAction<string>>;
  setComposerTaskNonce: React.Dispatch<React.SetStateAction<number>>;
  setJobNotice: React.Dispatch<React.SetStateAction<string | null>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
};

function buildGoalContractFromPrompt(promptRaw: string) {
  const lines = String(promptRaw || "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0);
  if (lines.length === 0) return null;
  const [goal, ...rest] = lines;
  const contract: Record<string, any> = { goal };
  if (rest.length > 0) contract.success_criteria = rest;
  return contract;
}

function collectTeamUploadFiles(attachments: Attachment[]) {
  return (attachments || []).filter((attachment) => {
    return typeof attachment?.data_base64 === "string" && attachment.data_base64.length > 0;
  });
}

export default function useTeamChatOrchestration(args: TeamChatOrchestrationArgs) {
  const {
    authKey,
    brokerAgentId,
    brokerBase,
    brokerChatAvailable,
    connectionMode,
    daemonAuth,
    selectedTeamId,
    setAdvancedPage,
    setComposerTaskNonce,
    setJobNotice,
    setPrompt,
  } = args;

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
  const teamQueueCount = Array.isArray(teamQueue) ? teamQueue.length : 0;
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
  const teamQueueBusyRef = React.useRef(false);

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
      const entries = Object.entries(memberSessions as Record<string, string>);
      const items: any[] = [];
      await Promise.all(
        entries.map(async ([memberId, sessionId]) => {
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
    teamQueueCount > 0 && !latestTeamRunId && teamQueue.some((entry) => entry.action !== "run");

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

  const teamRunCreate = useMutation({
    mutationFn: async (vars: { prompt: string }) => {
      const trimmed = String(vars.prompt || "").trim();
      if (!trimmed) throw new Error("prompt required");
      if (!brokerChatAvailable) throw new Error("team chat unavailable");
      if (!brokerBaseTrimmed) throw new Error("missing broker base");
      if (!selectedTeamIdTrimmed) throw new Error("missing team_id");
      return apiBrokerTeamRunCreate(brokerBaseTrimmed, selectedTeamIdTrimmed, { prompt: trimmed }, daemonAuth);
    },
  });

  const openTeamPanel = React.useCallback(
    (tab: string) => {
      if (connectionMode !== "broker") return;
      try {
        if (selectedTeamIdTrimmed) {
          window.localStorage.setItem("agentui.brokerTeamId", selectedTeamIdTrimmed);
        }
        if (tab) {
          window.localStorage.setItem("agentui.teamTab", tab);
        }
      } catch {
        // ignore localStorage failures
      }
      setAdvancedPage("broker");
    },
    [connectionMode, selectedTeamIdTrimmed, setAdvancedPage],
  );

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

  const waitForTeamMemberSessions = React.useCallback(
    async (runId: string) => {
      let lastStatus: any = null;
      for (let attempt = 0; attempt < 10; attempt += 1) {
        const status = await apiBrokerTeamRunGet(brokerBaseTrimmed, selectedTeamIdTrimmed, runId, daemonAuth);
        lastStatus = status;
        const sessions = status?.member_sessions;
        if (sessions && typeof sessions === "object" && Object.keys(sessions).length > 0) {
          return status;
        }
        await new Promise((resolve) => setTimeout(resolve, 750));
      }
      return lastStatus;
    },
    [brokerBaseTrimmed, daemonAuth, selectedTeamIdTrimmed],
  );

  const broadcastTeamUploads = React.useCallback(
    async (status: any, attachments: Attachment[]) => {
      const uploads = collectTeamUploadFiles(attachments);
      if (uploads.length === 0) return { uploads: [], errors: [] as string[] };
      const memberSessions = status?.member_sessions && typeof status.member_sessions === "object" ? status.member_sessions : {};
      const members = Array.isArray(status?.members) ? status.members : [];
      const memberJobs = Array.isArray(status?.member_jobs) ? status.member_jobs : [];
      const memberMeta: Record<string, { agent_id?: string; deployment_id?: string }> = {};
      for (const member of members) {
        const id = String((member as any)?.member_id || "").trim();
        if (!id) continue;
        memberMeta[id] = {
          agent_id: String((member as any)?.agent_id || "").trim() || undefined,
          deployment_id: String((member as any)?.deployment_id || "").trim() || undefined,
        };
      }
      for (const job of memberJobs) {
        const id = String((job as any)?.member_id || "").trim();
        if (!id) continue;
        memberMeta[id] = {
          agent_id: String((job as any)?.agent_id || memberMeta[id]?.agent_id || "").trim() || undefined,
          deployment_id: String((job as any)?.deployment_id || memberMeta[id]?.deployment_id || "").trim() || undefined,
        };
      }

      const errors: string[] = [];
      const uploadResults: any[] = [];
      const entries = Object.entries(memberSessions as Record<string, string>);
      for (const [memberIdRaw, sessionIdRaw] of entries) {
        const memberId = String(memberIdRaw || "").trim();
        const sid = String(sessionIdRaw || "").trim();
        if (!memberId || !sid) continue;
        const meta = memberMeta[memberId] || {};
        const agentId = meta.agent_id || brokerAgentId;
        if (!agentId) {
          errors.push(`missing agent_id for member ${memberId}`);
          continue;
        }
        const depId = meta.deployment_id;
        const body = {
          session_id: sid,
          files: uploads.map((file) => ({
            name: file.name || "upload.bin",
            mime: file.mime,
            data_base64: file.data_base64,
          })),
        };
        const resp = await apiBrokerProxyJson(
          brokerBaseTrimmed,
          agentId,
          "/api/v1/session/upload",
          "POST",
          body,
          daemonAuth,
          depId,
        );
        if (!resp?.data?.ok) {
          errors.push(`upload failed for member ${memberId}`);
          continue;
        }
        const files = Array.isArray(resp?.data?.files) ? resp.data.files : [];
        uploadResults.push({ member_id: memberId, session_id: sid, agent_id: agentId, deployment_id: depId, files });
      }
      return { uploads: uploadResults, errors };
    },
    [brokerAgentId, brokerBaseTrimmed, daemonAuth],
  );

  const runTeamAction = React.useCallback(
    async (vars: { prompt: string; attachments: Attachment[] }, action: TeamActionKind) => {
      const trimmed = String(vars.prompt || "").trim();
      if (!trimmed) {
        setJobNotice("prompt required");
        return;
      }

      if (action === "goal" && !latestTeamRunId) {
        setJobNotice("start a team run before setting a goal");
        return;
      }

      if (action === "guidance") {
        const uploads = collectTeamUploadFiles(vars.attachments);
        if (uploads.length > 0 && !latestTeamRunId) {
          setJobNotice("start a team run before attaching files");
          return;
        }
      }

      teamQueueBusyRef.current = true;
      try {
        if (action === "goal") {
          const contract = buildGoalContractFromPrompt(trimmed);
          if (!contract) {
            setJobNotice("goal required");
            return;
          }
          try {
            const resp = await apiBrokerTeamRunGoalUpdate(
              brokerBaseTrimmed,
              selectedTeamIdTrimmed,
              latestTeamRunId,
              { goal_contract: contract },
              daemonAuth,
            );
            if (!resp?.ok) throw new Error(resp?.error || resp?.err || "goal update failed");
            addTeamActivity({ ts: Date.now(), prompt: trimmed, run_id: latestTeamRunId, kind: "goal", payload: contract });
            setPrompt("");
            setComposerTaskNonce((count) => count + 1);
            setJobNotice("goal updated");
          } catch (error) {
            setJobNotice(`goal update failed: ${String(error)}`);
          }
          return;
        }

        if (action === "guidance") {
          try {
            const runId = latestTeamRunId || "";
            const uploads = collectTeamUploadFiles(vars.attachments);
            let uploadPayload: any = null;
            if (uploads.length > 0) {
              if (!runId) {
                setJobNotice("start a team run before attaching files");
                return;
              }
              setJobNotice("sharing attachments with the team...");
              const status = await waitForTeamMemberSessions(runId);
              const uploadResult = await broadcastTeamUploads(status, uploads);
              uploadPayload = { uploads: uploadResult.uploads };
              if (uploadResult.errors.length > 0) {
                setJobNotice(`shared with errors: ${uploadResult.errors[0]}`);
              }
            }
            const body: Record<string, any> = {
              kind: "note",
              priority: "normal",
              message: trimmed,
            };
            if (runId) body.team_run_id = runId;
            if (uploadPayload) body.payload = uploadPayload;
            const resp = await apiBrokerTeamGuidanceCreate(brokerBaseTrimmed, selectedTeamIdTrimmed, body, daemonAuth);
            if (!resp?.ok) throw new Error(resp?.error || resp?.err || "guidance failed");
            addTeamActivity({ ts: Date.now(), prompt: trimmed, run_id: runId || undefined, kind: "guidance", payload: uploadPayload });
            setPrompt("");
            setComposerTaskNonce((count) => count + 1);
            setJobNotice("guidance sent");
            if (teamGuidance.refetch) void teamGuidance.refetch();
          } catch (error) {
            setJobNotice(`guidance failed: ${String(error)}`);
          }
          return;
        }

        try {
          const resp = await teamRunCreate.mutateAsync({ prompt: trimmed });
          if (!resp?.ok) throw new Error(resp?.error || resp?.err || resp?.code || "team run failed");
          const runId = String(resp?.team_run_id || "").trim();
          addTeamActivity({ ts: Date.now(), prompt: trimmed, run_id: runId || undefined, kind: "prompt" });
          setPrompt("");
          setComposerTaskNonce((count) => count + 1);
          setJobNotice(runId ? `team run ${runId} started` : "team run started");
          if (teamRunList.refetch) void teamRunList.refetch();
          const uploads = collectTeamUploadFiles(vars.attachments);
          if (uploads.length > 0) {
            if (!runId) {
              setJobNotice("team run id missing; attachments not shared");
              return;
            }
            setJobNotice("sharing attachments with the team...");
            const status = await waitForTeamMemberSessions(runId);
            const uploadResult = await broadcastTeamUploads(status, uploads);
            const fileNames = uploads.map((file) => file.name || "file").join(", ");
            const guidanceBody: Record<string, any> = {
              kind: "resource",
              priority: "normal",
              message: `Shared files: ${fileNames}`,
              team_run_id: runId,
              payload: { uploads: uploadResult.uploads },
            };
            const guidanceResp = await apiBrokerTeamGuidanceCreate(
              brokerBaseTrimmed,
              selectedTeamIdTrimmed,
              guidanceBody,
              daemonAuth,
            );
            if (guidanceResp?.ok) {
              addTeamActivity({
                ts: Date.now(),
                prompt: String(guidanceBody.message || ""),
                run_id: runId,
                kind: "guidance",
                payload: guidanceBody.payload,
              });
            }
            if (uploadResult.errors.length > 0) {
              setJobNotice(`shared with errors: ${uploadResult.errors[0]}`);
            } else {
              setJobNotice("attachments shared");
            }
            setComposerTaskNonce((count) => count + 1);
            if (teamGuidance.refetch) void teamGuidance.refetch();
          }
        } catch (error) {
          setJobNotice(`team run failed: ${String(error)}`);
        }
      } finally {
        teamQueueBusyRef.current = false;
      }
    },
    [
      addTeamActivity,
      broadcastTeamUploads,
      brokerBaseTrimmed,
      daemonAuth,
      latestTeamRunId,
      selectedTeamIdTrimmed,
      setComposerTaskNonce,
      setJobNotice,
      setPrompt,
      teamGuidance,
      teamRunCreate,
      teamRunList,
      waitForTeamMemberSessions,
    ],
  );

  const enqueueTeamAction = React.useCallback(
    (vars: { prompt: string; attachments: Attachment[] }, action: TeamActionKind) => {
      const trimmed = String(vars.prompt || "").trim();
      if (!trimmed && vars.attachments.length === 0) {
        setJobNotice("prompt or attachment required");
        return;
      }
      setTeamQueue((prev) => [
        ...(Array.isArray(prev) ? prev : []),
        { prompt: trimmed, attachments: vars.attachments, queued_unix_ms: Date.now(), action },
      ]);
      setPrompt("");
      setComposerTaskNonce((count) => count + 1);
      setJobNotice(`queued team ${action}`);
    },
    [setComposerTaskNonce, setJobNotice, setPrompt, setTeamQueue],
  );

  React.useEffect(() => {
    if (!brokerChatAvailable) return;
    if (teamQueueBusyRef.current) return;
    if (!Array.isArray(teamQueue) || teamQueue.length === 0) return;
    const next = teamQueue[0];
    if (!next) return;
    if (next.action !== "run" && !latestTeamRunId) return;
    setTeamQueue((prev) => (Array.isArray(prev) ? prev.slice(1) : []));
    runTeamAction({ prompt: next.prompt, attachments: next.attachments }, next.action)
      .catch((error) => {
        setJobNotice(`queued team ${next.action} failed: ${String(error)}`);
        setTeamQueue((prev) => [next, ...(Array.isArray(prev) ? prev : [])]);
      })
      .finally(() => {
        teamQueueBusyRef.current = false;
      });
  }, [brokerChatAvailable, latestTeamRunId, runTeamAction, setJobNotice, setTeamQueue, teamQueue]);

  const handleTeamRunRequest = React.useCallback(
    async (vars: { prompt: string; attachments: Attachment[] }, action: TeamActionKind) => {
      if (teamQueueBusyRef.current || teamRunCreate.isPending) {
        enqueueTeamAction(vars, action);
        return;
      }
      await runTeamAction(vars, action);
    },
    [enqueueTeamAction, runTeamAction, teamRunCreate.isPending],
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
  const clearTeamQueue = React.useCallback(() => setTeamQueue([]), [setTeamQueue]);

  return {
    clearTeamQueue,
    handleTeamRunRequest,
    latestTeamRunCreatedMs,
    latestTeamRunId,
    openTeamPanel,
    teamConversationCacheUpdatedMs,
    teamConversationItems,
    teamConversationUsingCache,
    teamConversationWarnings,
    teamQueue,
    teamQueueCount,
    teamQueueNeedsRun,
    teamRecentActivity,
    teamRunCreate,
    teamStatus,
  };
}
