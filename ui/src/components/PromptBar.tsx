import React from "react";
import { apiCancelJob, apiPostSessionUpload, type ApiAuth } from "../api";
import useLocalStorageState from "../hooks/useLocalStorageState";

export type Attachment = {
  path: string;
  name?: string;
  mime?: string;
  kind?: string;
  bytes?: number;
  data_base64?: string;
};

function guessMimeFromName(name: string): string {
  const lower = String(name || "").toLowerCase();
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".svg")) return "image/svg+xml";
  if (lower.endsWith(".mp3")) return "audio/mpeg";
  if (lower.endsWith(".wav")) return "audio/wav";
  if (lower.endsWith(".mp4")) return "video/mp4";
  if (lower.endsWith(".webm")) return "video/webm";
  if (lower.endsWith(".mov")) return "video/quicktime";
  if (lower.endsWith(".txt") || lower.endsWith(".md")) return "text/plain";
  return "";
}

function sanitizeUploadName(name: string): string {
  let out = String(name || "").trim();
  if (!out) return "upload.bin";
  out = out.replace(/[\\/]/g, "_");
  out = out.replace(/[^A-Za-z0-9._-]/g, "_");
  out = out.replace(/_+/g, "_");
  if (out.length > 200) out = out.slice(0, 200);
  if (out === "." || out === ".." || out === "") return "upload.bin";
  if (out.includes("..")) out = out.replace(/\.\.+/g, ".");
  return out;
}

function isSafeSessionId(value: string): boolean {
  if (!value) return false;
  if (value.length > 200) return false;
  if (value === "." || value === "..") return false;
  if (value.includes("..")) return false;
  return /^[A-Za-z0-9._-]+$/.test(value);
}

async function fileToBase64(file: File): Promise<string> {
  return await new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onload = () => {
      const res = String(fr.result ?? "");
      const idx = res.indexOf(",");
      resolve(idx >= 0 ? res.slice(idx + 1) : res);
    };
    fr.onerror = () => reject(fr.error || new Error("FileReader failed"));
    fr.readAsDataURL(file);
  });
}

type PromptBarProps = {
  effectiveBase: string;
  sessionId: string | null | undefined;
  tools: string;
  activeJobId: string | null;
  jobStatus: string | null;
  jobProgressLabel: string;
  runWatchMode: string;
  daemonAuth: ApiAuth;
  prompt: string;
  setPrompt: (next: string) => void;
  runDisabled: boolean;
  runLabel: string;
  queueCount?: number;
  onRun: (vars: { prompt: string; attachments: Attachment[] }) => void;
  setJobNotice: (next: string | null) => void;
  jobNotice: string | null;
  jobError: string | null;
  runError: string | null;
  resultError: string | null;
  clearAttachmentsNonce: number;
  uploadsEnabled?: boolean;
  uploadMaxBytes?: number;
  uploadsDisabledReason?: string;
  chatTarget?: "session" | "team";
  teamId?: string;
  teamAvailable?: boolean;
  onChatTargetChange?: (next: "session" | "team") => void;
  uploadMode?: "session" | "team";
  teamAction?: "run" | "guidance" | "goal";
  onTeamActionChange?: (next: "run" | "guidance" | "goal") => void;
};

const PromptBar = React.forwardRef<HTMLDivElement, PromptBarProps>(function PromptBar(props, ref) {
  const [attachments, setAttachments] = React.useState<Attachment[]>([]);
  const [drawerOpen, setDrawerOpen] = React.useState<boolean>(false);
  const [uploadBusy, setUploadBusy] = React.useState<boolean>(false);
  const collapsedKey = React.useMemo(() => `agentui.composer.collapsed:${String(props.effectiveBase || "").trim() || "default"}`, [props.effectiveBase]);
  const [collapsed, setCollapsed] = useLocalStorageState<boolean>(collapsedKey, false);

  const promptPreview = React.useMemo(() => {
    const p = String(props.prompt || "").trim();
    if (!p) return "";
    return p.length > 140 ? `${p.slice(0, 140)}…` : p;
  }, [props.prompt]);
  const uploadsEnabled = props.uploadsEnabled !== false;
  const uploadsDisabledReason = String(props.uploadsDisabledReason || "").trim();
  const uploadMaxBytes =
    typeof props.uploadMaxBytes === "number" && Number.isFinite(props.uploadMaxBytes) && props.uploadMaxBytes > 0
      ? props.uploadMaxBytes
      : 32 * 1024 * 1024;
  const chatTarget = props.chatTarget === "team" ? "team" : "session";
  const teamAvailable = props.teamAvailable === true;
  const teamId = String(props.teamId || "").trim();
  const uploadMode = props.uploadMode === "team" ? "team" : "session";
  const teamAction = props.teamAction === "guidance" || props.teamAction === "goal" ? props.teamAction : "run";

  React.useEffect(() => {
    setAttachments([]);
    setDrawerOpen(false);
    setUploadBusy(false);
  }, [props.clearAttachmentsNonce]);

  // Session changes should never carry a pending "next run" attachment set.
  React.useEffect(() => {
    setAttachments([]);
    setDrawerOpen(false);
  }, [props.effectiveBase, String(props.sessionId || "").trim()]);

  React.useEffect(() => {
    setAttachments([]);
  }, [chatTarget, teamAction]);

  React.useEffect(() => {
    const shouldOpen =
      !!props.jobError ||
      !!props.runError ||
      !!props.resultError;
    if (shouldOpen) setDrawerOpen(true);
  }, [props.jobError, props.runError, props.resultError]);

  React.useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setDrawerOpen(false);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const removeAttachment = React.useCallback((path: string) => {
    const p = String(path || "").trim();
    if (!p) return;
    setAttachments((prev) => prev.filter((a) => a.path !== p));
  }, []);

  const onRun = React.useCallback(() => {
    const vars = { prompt: props.prompt, attachments };
    props.onRun(vars);
  }, [attachments, props]);

  const promptPlaceholder = React.useMemo(() => {
    if (chatTarget === "team") {
      if (teamAction === "guidance") return "Add a guideline or update for the team…";
      if (teamAction === "goal") return "Describe the goal (first line), add success criteria on new lines…";
      return "Describe the team task…";
    }
    return "Describe the task… (Ctrl/⌘+Enter to run)";
  }, [chatTarget, teamAction]);

  return (
    <div ref={ref} className="fixed bottom-0 left-0 right-0 z-30 border-t border-white/10 bg-slate-950/90 backdrop-blur">
      {drawerOpen ? (
        <div
          className="fixed inset-0 z-30"
          onClick={() => setDrawerOpen(false)}
          aria-hidden="true"
        />
      ) : null}

      {/* Drawer (does not affect promptbar height). */}
      {drawerOpen ? (
        <div className="absolute bottom-full left-0 right-0 z-40">
          <div className="mx-auto max-w-7xl px-3 pb-2">
            <div className="max-h-[60vh] overflow-y-auto rounded-lg border border-white/10 bg-slate-950/95 shadow-xl">
              <div className="flex items-center justify-between gap-3 border-b border-white/10 px-3 py-2">
                <div className="text-xs font-semibold text-white/80">Details</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setDrawerOpen(false)}
                >
                  Close
                </button>
              </div>

              <div className="grid gap-3 px-3 py-3 text-xs text-white/70">
                <div className="text-[11px] text-white/60">
                  session=<code className="text-white/70 break-all">{String(props.sessionId || "").trim() || "(none)"}</code>{" "}
                  tools=<code className="text-white/70 break-all">{String(props.tools || "")}</code>{" "}
                  run_watch=<code className="text-white/70 break-all">{String(props.runWatchMode || "local")}</code>{" "}
                  target=<code className="text-white/70 break-all">{chatTarget}</code>{" "}
                  {chatTarget === "team" ? (
                    <>
                      action=<code className="text-white/70 break-all">{teamAction}</code>{" "}
                    </>
                  ) : null}
                  {props.queueCount && props.queueCount > 0 ? (
                    <>
                      queued=<code className="text-white/70 break-all">{props.queueCount}</code>{" "}
                    </>
                  ) : null}
                  {props.activeJobId ? (
                    <>
                      job=<code className="text-white/70 break-all">{props.activeJobId}</code>{" "}
                      status=<code className="text-white/70 break-all">{props.jobStatus ?? "running"}</code>{" "}
                      {props.jobProgressLabel ? (
                        <>
                          phase=<code className="text-white/70 break-all">{props.jobProgressLabel}</code>
                        </>
                      ) : null}
                    </>
                  ) : null}
                </div>

                <div>
                  <div className="flex items-center justify-between gap-2">
                    <div className="font-medium text-white/70">Attachments</div>
                    {attachments.length > 0 ? (
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => setAttachments([])}
                      >
                        Clear ({attachments.length})
                      </button>
                    ) : null}
                  </div>

                  {attachments.length === 0 ? (
                    <div className="mt-1 text-white/50">No attachments staged for the next run.</div>
                  ) : (
                    <div className="mt-2 max-h-48 overflow-y-auto rounded-md border border-white/10 bg-black/20 p-2">
                      <div className="flex flex-wrap items-center gap-2">
                        {attachments.map((a) => (
                          <div
                            key={a.path}
                            className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1"
                            title={a.path}
                          >
                            <span className="max-w-[360px] truncate text-white/80">{a.name || a.path}</span>
                            <button
                              className="text-white/60 hover:text-white"
                              type="button"
                              onClick={() => removeAttachment(a.path)}
                              aria-label={`Remove ${a.name || a.path}`}
                            >
                              ×
                            </button>
                          </div>
                        ))}
                      </div>
                    </div>
                  )}

                  <div className="mt-2 text-white/50">
                    Attachments apply to the next <span className="text-white/70">Run</span> only. After the run is accepted by the daemon (async) or completes (sync), the staged list is cleared.
                    Uploaded files may still remain in the session storage on the daemon.
                  </div>
                  {uploadMode === "team" ? (
                    <div className="mt-2 text-white/50">Team attachments are shared with all team members when you send.</div>
                  ) : null}
                </div>

                {props.runError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                    Failed: {props.runError}
                  </div>
                ) : null}
                {props.jobError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                    {props.jobError}
                  </div>
                ) : null}
                {props.jobNotice ? (
                  <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
                    {props.jobNotice}
                  </div>
                ) : null}
                {props.resultError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                    {props.resultError}
                  </div>
                ) : null}
              </div>
            </div>
          </div>
        </div>
      ) : null}

      <div className="mx-auto max-w-7xl px-3 py-3">
        <div className="flex min-w-0 items-center justify-between gap-2">
          <div className="flex min-w-0 items-center gap-2 text-[11px] text-white/60">
            <button
              className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => setCollapsed((v) => !v)}
              aria-expanded={!collapsed}
              title={collapsed ? "Expand composer" : "Collapse composer"}
            >
              {collapsed ? "Expand" : "Collapse"}
              {collapsed && promptPreview ? <span className="max-w-[48vw] truncate text-white/50">{promptPreview}</span> : null}
              {attachments.length > 0 ? (
                <span className="text-white/50">
                  ({attachments.length} attachment{attachments.length === 1 ? "" : "s"})
                </span>
              ) : null}
            </button>

            <button
              className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => setDrawerOpen((v) => !v)}
              aria-expanded={drawerOpen}
            >
              {drawerOpen ? "Hide details" : "Show details"}
              {props.jobError || props.runError || props.resultError ? <span className="text-rose-300">•</span> : null}
              {props.jobNotice ? <span className="text-amber-200">•</span> : null}
            </button>

            {teamAvailable ? (
              <div className="inline-flex items-center overflow-hidden rounded-md border border-white/10 bg-black/30 text-xs text-white/70">
                <button
                  className={`px-2 py-1 ${chatTarget === "session" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onChatTargetChange?.("session")}
                >
                  Session
                </button>
                <button
                  className={`px-2 py-1 ${chatTarget === "team" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onChatTargetChange?.("team")}
                >
                  Team {teamId ? `· ${teamId}` : ""}
                </button>
              </div>
            ) : null}

            {chatTarget === "team" ? (
              <div className="inline-flex items-center overflow-hidden rounded-md border border-white/10 bg-black/30 text-xs text-white/70">
                <button
                  className={`px-2 py-1 ${teamAction === "run" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("run")}
                >
                  Prompt
                </button>
                <button
                  className={`px-2 py-1 ${teamAction === "guidance" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("guidance")}
                >
                  Guidance
                </button>
                <button
                  className={`px-2 py-1 ${teamAction === "goal" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("goal")}
                >
                  Goal
                </button>
              </div>
            ) : null}
          </div>

          <div className="flex items-center gap-2">
            {props.activeJobId ? (
              <button
                className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15"
                onClick={async () => {
                  const jobId = props.activeJobId;
                  if (!jobId) return;
                  try {
                    await apiCancelJob(props.effectiveBase, jobId, props.daemonAuth);
                    props.setJobNotice("cancel requested");
                  } catch (e) {
                    props.setJobNotice(`cancel failed: ${String(e)}`);
                  }
                }}
                type="button"
              >
                Cancel
              </button>
            ) : null}
            <button
              className="rounded-md bg-indigo-500 px-4 py-2 text-sm font-semibold text-white hover:bg-indigo-400 disabled:opacity-50"
              onClick={onRun}
              disabled={props.runDisabled}
              type="button"
              data-testid="run"
            >
              {props.runLabel}
            </button>
            {props.queueCount && props.queueCount > 0 ? (
              <span className="rounded-md border border-indigo-400/30 bg-indigo-500/10 px-2 py-1 text-xs text-indigo-100">
                {props.queueCount} queued
              </span>
            ) : null}
          </div>
        </div>

        {!collapsed ? (
          <>
            <textarea
              className="mt-2 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm leading-relaxed shadow-inner max-h-[30vh] overflow-y-auto"
              data-testid="prompt"
              rows={3}
              value={props.prompt}
              placeholder={promptPlaceholder}
              onChange={(e) => props.setPrompt(e.target.value)}
              onKeyDown={(e) => {
                if (props.runDisabled) return;
                if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
                  e.preventDefault();
                  onRun();
                }
              }}
            />

            <div className="mt-2 flex flex-wrap items-center gap-2">
              <input
                type="file"
                multiple
                className="hidden"
                id="agentui-file-input"
                disabled={!uploadsEnabled}
                onChange={async (e) => {
                  if (!uploadsEnabled) {
                    props.setJobNotice("uploads disabled by daemon caps");
                    return;
                  }
                  const files = Array.from(e.target.files || []);
                  e.currentTarget.value = "";
                  if (files.length === 0) return;
                  if (uploadBusy) return;
                  const rawSid = String(props.sessionId || "").trim();
                  const sid = isSafeSessionId(rawSid) ? rawSid : "default";
                  setUploadBusy(true);
                  props.setJobNotice(null);
                  try {
                    const payloadFiles: { name: string; mime?: string; data_base64: string; bytes?: number }[] = [];
                    for (const f of files.slice(0, 16)) {
                      if (typeof (f as any)?.size === "number" && (f as any).size > uploadMaxBytes) continue;
                      const name = sanitizeUploadName(f.name || "upload.bin");
                      const mime = String(f.type || "").trim() || guessMimeFromName(name) || undefined;
                      const data_base64 = await fileToBase64(f);
                      payloadFiles.push({ name, mime, data_base64, bytes: (f as any)?.size });
                    }
                    if (payloadFiles.length === 0) {
                      props.setJobNotice("no files uploaded (too large or invalid)");
                      return;
                    }
                    if (uploadMode === "team") {
                      const newOnes = payloadFiles.map((f) => ({
                        path: `local:${Date.now()}-${Math.random().toString(16).slice(2)}`,
                        name: f.name,
                        mime: f.mime,
                        kind: f.mime,
                        bytes: typeof f.bytes === "number" ? f.bytes : undefined,
                        data_base64: f.data_base64,
                      }));
                      setAttachments((prev) => [...prev, ...newOnes]);
                      props.setJobNotice(`staged ${newOnes.length} file(s) for team`);
                      return;
                    }

                    const resp = await apiPostSessionUpload(
                      props.effectiveBase,
                      { session_id: sid, files: payloadFiles.map(({ name, mime, data_base64 }) => ({ name, mime, data_base64 })) },
                      props.daemonAuth,
                    );
                    if (!resp.ok) {
                      const errors = (resp as any)?.errors;
                      if (Array.isArray(errors) && errors.length > 0) {
                        const err0 = errors[0] || {};
                        const code = typeof err0?.code === "string" ? err0.code : "";
                        const msg = typeof err0?.error === "string" ? err0.error : "";
                        const name = typeof err0?.name === "string" ? err0.name : "";
                        const detail = [code, msg].filter(Boolean).join(": ");
                        props.setJobNotice(`upload failed${name ? ` (${name})` : ""}: ${detail || "upload failed"}`);
                      } else {
                        props.setJobNotice(resp.error ? `upload failed: ${resp.error}` : "upload failed");
                      }
                      return;
                    }
                    const newOnes =
                      (resp.files || [])
                        .map((x: any) => {
                          const path = typeof x?.path === "string" ? x.path : "";
                          if (!path) return null;
                          return {
                            path,
                            name: typeof x?.name === "string" ? x.name : undefined,
                            mime: typeof x?.mime === "string" ? x.mime : undefined,
                            kind: typeof x?.kind === "string" ? x.kind : undefined,
                            bytes: typeof x?.bytes === "number" ? x.bytes : undefined,
                          } as Attachment;
                        })
                        .filter(Boolean) as Attachment[];
                    if (newOnes.length === 0) {
                      props.setJobNotice("upload succeeded but returned no file paths");
                      return;
                    }
                    setAttachments((prev) => {
                      const merged = [...prev];
                      for (const a of newOnes) {
                        if (!merged.some((m) => m.path === a.path)) merged.push(a);
                      }
                      return merged;
                    });
                    props.setJobNotice(`uploaded ${newOnes.length} file(s)`);
                  } catch (err) {
                    props.setJobNotice(`upload failed: ${String(err)}`);
                  } finally {
                    setUploadBusy(false);
                  }
                }}
              />
              <label
                htmlFor="agentui-file-input"
                className={`inline-flex cursor-pointer items-center gap-2 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 ${
                  uploadBusy || !uploadsEnabled ? "opacity-50 pointer-events-none" : ""
                }`}
              >
                {uploadBusy ? "Uploading…" : "Attach files"}
              </label>
              {!uploadsEnabled ? (
                <div className="text-xs text-rose-200">{uploadsDisabledReason || "Uploads disabled by daemon caps."}</div>
              ) : null}

              {attachments.length > 0 ? (
                <div className="text-xs text-white/60">
                  Staged: <span className="text-white/80">{attachments.length}</span>
                </div>
              ) : (
                <div className="text-xs text-white/50">No staged attachments</div>
              )}
            </div>
          </>
        ) : null}
      </div>
    </div>
  );
});

export default PromptBar;
