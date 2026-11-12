import React from "react";

import { apiCancelJob, apiPostSessionUpload } from "../../api";
import { safeObject } from "../../jsonUtils";
import type { Attachment, PromptBarProps } from "./promptBarTypes";
import { fileToBase64, guessMimeFromName, isSafeSessionId, sanitizeUploadName } from "./promptBarUtils";

function errorMessage(error: unknown, fallback: string): string {
  if (error instanceof Error && error.message) return error.message;
  if (typeof error === "string" && error.trim()) return error;
  return fallback;
}

export default function usePromptBarState(props: PromptBarProps) {
  const [attachments, setAttachments] = React.useState<Attachment[]>([]);
  const [uploadBusy, setUploadBusy] = React.useState<boolean>(false);
  const [cancelRequest, setCancelRequest] = React.useState<{
    jobId: string;
    phase: "sending" | "requested" | "error";
    error?: string;
  } | null>(null);
  React.useEffect(() => setCancelRequest(null), [props.activeJobId]);
  const currentCancelRequest = cancelRequest?.jobId === props.activeJobId ? cancelRequest : null;
  const cancelPending = currentCancelRequest?.phase === "sending" || currentCancelRequest?.phase === "requested";
  const cancelError = currentCancelRequest?.phase === "error" ? currentCancelRequest.error : null;
  const uploadsEnabled = props.uploadsEnabled !== false;
  const uploadsDisabledReason = String(props.uploadsDisabledReason || "").trim();
  const uploadMaxBytes =
    typeof props.uploadMaxBytes === "number" && Number.isFinite(props.uploadMaxBytes) && props.uploadMaxBytes > 0
      ? props.uploadMaxBytes
      : 32 * 1024 * 1024;
  const chatTarget: "session" | "team" = props.chatTarget === "team" ? "team" : "session";
  const teamAvailable = props.teamAvailable === true;
  const teamId = String(props.teamId || "").trim();
  const uploadMode: "session" | "team" = props.uploadMode === "team" ? "team" : "session";
  const teamAction: "run" | "guidance" | "goal" =
    props.teamAction === "guidance" || props.teamAction === "goal" ? props.teamAction : "run";

  React.useEffect(() => {
    setAttachments([]);
    setUploadBusy(false);
  }, [props.clearAttachmentsNonce]);

  React.useEffect(() => {
    setAttachments([]);
  }, [props.effectiveBase, String(props.sessionId || "").trim()]);

  React.useEffect(() => {
    setAttachments([]);
  }, [chatTarget, teamAction]);

  const removeAttachment = React.useCallback((path: string) => {
    const trimmed = String(path || "").trim();
    if (!trimmed) return;
    setAttachments((prev) => prev.filter((item) => item.path !== trimmed));
  }, []);

  const onRun = React.useCallback(() => {
    if (props.composerDisabled || props.runDisabled) return;
    props.onRun({ prompt: props.prompt, attachments });
  }, [attachments, props]);

  const promptPlaceholder = React.useMemo(() => {
    if (chatTarget === "team") {
      if (teamAction === "guidance") return "Add a guideline or update for the team…";
      if (teamAction === "goal") return "Describe the goal (first line), add success criteria on new lines…";
      return "Describe the team task…";
    }
    return "Message your agent…";
  }, [chatTarget, teamAction]);

  const handleCancel = React.useCallback(async () => {
    const jobId = props.activeJobId;
    if (!jobId || cancelPending) return;
    setCancelRequest({ jobId, phase: "sending" });
    try {
      const response = await apiCancelJob(props.effectiveBase, jobId, props.daemonAuth);
      if (response.ok !== true) {
        throw new Error(response.error || response.err || response.code || `HTTP ${response.status ?? "error"}`);
      }
      setCancelRequest((current) => current?.jobId === jobId ? { jobId, phase: "requested" } : current);
    } catch (error) {
      setCancelRequest((current) => current?.jobId === jobId
        ? { jobId, phase: "error", error: errorMessage(error, "Cancellation failed") }
        : current);
    }
  }, [cancelPending, props]);

  const handleUploadFiles = React.useCallback(
    async (files: FileList | File[]) => {
      if (props.composerDisabled) return;
      if (!uploadsEnabled) {
        props.setJobNotice("uploads disabled by daemon caps");
        return;
      }
      const list = Array.from(files || []);
      if (list.length === 0 || uploadBusy) return;
      const rawSid = String(props.sessionId || "").trim();
      const sid = isSafeSessionId(rawSid) ? rawSid : "default";
      setUploadBusy(true);
      props.setJobNotice(null);
      try {
        const payloadFiles: { name: string; mime?: string; data_base64: string; bytes?: number }[] = [];
        for (const file of list.slice(0, 16)) {
          if (typeof file.size === "number" && file.size > uploadMaxBytes) continue;
          const name = sanitizeUploadName(file.name || "upload.bin");
          const mime = String(file.type || "").trim() || guessMimeFromName(name) || undefined;
          const data_base64 = await fileToBase64(file);
          payloadFiles.push({ name, mime, data_base64, bytes: file.size });
        }
        if (payloadFiles.length === 0) {
          props.setJobNotice("no files uploaded (too large or invalid)");
          return;
        }
        if (uploadMode === "team") {
          const newAttachments = payloadFiles.map((file) => ({
            path: `local:${Date.now()}-${Math.random().toString(16).slice(2)}`,
            name: file.name,
            mime: file.mime,
            kind: file.mime,
            bytes: typeof file.bytes === "number" ? file.bytes : undefined,
            data_base64: file.data_base64,
          }));
          setAttachments((prev) => [...prev, ...newAttachments]);
          props.setJobNotice(`staged ${newAttachments.length} file(s) for team`);
          return;
        }

        const resp = await apiPostSessionUpload(
          props.effectiveBase,
          { session_id: sid, files: payloadFiles.map(({ name, mime, data_base64 }) => ({ name, mime, data_base64 })) },
          props.daemonAuth,
        );
        if (!resp.ok) {
          const respRecord = safeObject(resp);
          const errors = Array.isArray(respRecord.errors) ? respRecord.errors : [];
          if (Array.isArray(errors) && errors.length > 0) {
            const err0 = safeObject(errors[0]);
            const code = typeof err0.code === "string" ? err0.code : "";
            const msg = typeof err0.error === "string" ? err0.error : "";
            const name = typeof err0.name === "string" ? err0.name : "";
            const detail = [code, msg].filter(Boolean).join(": ");
            props.setJobNotice(`upload failed${name ? ` (${name})` : ""}: ${detail || "upload failed"}`);
          } else {
            props.setJobNotice(resp.error ? `upload failed: ${resp.error}` : "upload failed");
          }
          return;
        }

        const uploaded =
          (resp.files || [])
            .map((item): Attachment | null => {
              const path = typeof item.path === "string" ? item.path : "";
              if (!path) return null;
              return {
                path,
                name: typeof item.name === "string" ? item.name : undefined,
                mime: typeof item.mime === "string" ? item.mime : undefined,
                kind: typeof item.kind === "string" ? item.kind : undefined,
                bytes: typeof item.bytes === "number" ? item.bytes : undefined,
              };
            })
            .filter((item): item is Attachment => item !== null);
        if (uploaded.length === 0) {
          props.setJobNotice("upload succeeded but returned no file paths");
          return;
        }
        setAttachments((prev) => {
          const merged = [...prev];
          for (const item of uploaded) {
            if (!merged.some((existing) => existing.path === item.path)) merged.push(item);
          }
          return merged;
        });
        props.setJobNotice(`uploaded ${uploaded.length} file(s)`);
      } catch (error: unknown) {
        props.setJobNotice(`upload failed: ${errorMessage(error, "upload failed")}`);
      } finally {
        setUploadBusy(false);
      }
    },
    [props, uploadBusy, uploadMaxBytes, uploadMode, uploadsEnabled],
  );

  return {
    attachments,
    cancelError,
    cancelPending,
    chatTarget,
    onRun,
    promptPlaceholder,
    removeAttachment,
    setAttachments,
    teamAction,
    teamAvailable,
    teamId,
    uploadBusy,
    uploadMaxBytes,
    uploadMode,
    uploadsDisabledReason,
    uploadsEnabled,
    handleCancel,
    handleUploadFiles,
  };
}
