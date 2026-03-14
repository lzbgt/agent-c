import React from "react";
import { useMutation } from "@tanstack/react-query";
import {
  apiBrokerProxyJson,
  apiBrokerTeamGuidanceCreate,
  apiBrokerTeamRunCreate,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunGoalUpdate,
  type ApiAuth,
} from "../api";
import type { Attachment } from "../components/PromptBar";
import type {
  TeamActionKind,
  TeamActivity,
  TeamQueuedAction,
  TeamRunRequest,
} from "./teamChatOrchestrationTypes";
import { buildGoalContractFromPrompt, collectTeamUploadFiles } from "./teamChatOrchestrationUtils";

type TeamChatActionStateArgs = {
  addTeamActivity: (entry: TeamActivity) => void;
  brokerAgentId: string;
  brokerBaseTrimmed: string;
  brokerChatAvailable: boolean;
  daemonAuth: ApiAuth;
  latestTeamRunId: string;
  refetchTeamGuidance?: () => unknown;
  refetchTeamRuns?: () => unknown;
  selectedTeamIdTrimmed: string;
  setComposerTaskNonce: React.Dispatch<React.SetStateAction<number>>;
  setJobNotice: React.Dispatch<React.SetStateAction<string | null>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
  setTeamQueue: React.Dispatch<React.SetStateAction<TeamQueuedAction[]>>;
  teamQueue: TeamQueuedAction[];
};

export function useTeamChatActionState(args: TeamChatActionStateArgs) {
  const {
    addTeamActivity,
    brokerAgentId,
    brokerBaseTrimmed,
    brokerChatAvailable,
    daemonAuth,
    latestTeamRunId,
    refetchTeamGuidance,
    refetchTeamRuns,
    selectedTeamIdTrimmed,
    setComposerTaskNonce,
    setJobNotice,
    setPrompt,
    setTeamQueue,
    teamQueue,
  } = args;

  const teamQueueBusyRef = React.useRef(false);
  const teamQueueEntries = Array.isArray(teamQueue) ? teamQueue : [];

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

      const memberSessions =
        status?.member_sessions && typeof status.member_sessions === "object" ? status.member_sessions : {};
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
      for (const [memberIdRaw, sessionIdRaw] of Object.entries(memberSessions as Record<string, string>)) {
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
    async (vars: TeamRunRequest, action: TeamActionKind) => {
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
            void refetchTeamGuidance?.();
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
          void refetchTeamRuns?.();

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
            void refetchTeamGuidance?.();
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
      refetchTeamGuidance,
      refetchTeamRuns,
      selectedTeamIdTrimmed,
      setComposerTaskNonce,
      setJobNotice,
      setPrompt,
      teamRunCreate,
      waitForTeamMemberSessions,
    ],
  );

  const enqueueTeamAction = React.useCallback(
    (vars: TeamRunRequest, action: TeamActionKind) => {
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
    if (teamQueueEntries.length === 0) return;
    const next = teamQueueEntries[0];
    if (!next) return;
    if (next.action !== "run" && !latestTeamRunId) return;

    setTeamQueue((prev) => (Array.isArray(prev) ? prev.slice(1) : []));
    runTeamAction({ prompt: next.prompt, attachments: next.attachments }, next.action).catch((error) => {
      setJobNotice(`queued team ${next.action} failed: ${String(error)}`);
      setTeamQueue((prev) => [next, ...(Array.isArray(prev) ? prev : [])]);
    });
  }, [brokerChatAvailable, latestTeamRunId, runTeamAction, setJobNotice, setTeamQueue, teamQueueEntries]);

  const handleTeamRunRequest = React.useCallback(
    async (vars: TeamRunRequest, action: TeamActionKind) => {
      if (teamQueueBusyRef.current || teamRunCreate.isPending) {
        enqueueTeamAction(vars, action);
        return;
      }
      await runTeamAction(vars, action);
    },
    [enqueueTeamAction, runTeamAction, teamRunCreate.isPending],
  );

  const clearTeamQueue = React.useCallback(() => setTeamQueue([]), [setTeamQueue]);

  return {
    clearTeamQueue,
    handleTeamRunRequest,
    teamRunCreate,
  };
}
