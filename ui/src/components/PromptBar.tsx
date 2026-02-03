import React from "react";
import { apiCancelJob, apiPostSessionUpload } from "../api";

export type Attachment = { path: string; name?: string; mime?: string; kind?: string; bytes?: number };

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
  daemonAuthToken: string;
  prompt: string;
  setPrompt: (next: string) => void;
  runDisabled: boolean;
  runLabel: string;
  onRun: (vars: { prompt: string; attachments: Attachment[] }) => void;
  setJobNotice: (next: string | null) => void;
  jobNotice: string | null;
  jobError: string | null;
  runError: string | null;
  resultError: string | null;
  clearAttachmentsNonce: number;
};

const PromptBar = React.forwardRef<HTMLDivElement, PromptBarProps>(function PromptBar(props, ref) {
  const [attachments, setAttachments] = React.useState<Attachment[]>([]);
  const [drawerOpen, setDrawerOpen] = React.useState<boolean>(false);
  const [uploadBusy, setUploadBusy] = React.useState<boolean>(false);

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
                    Attachments apply to the next <span className="text-white/70">Run</span> only. After the run starts, the staged list is cleared.
                    Uploaded files may still remain in the session storage on the daemon.
                  </div>
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
          <div className="min-w-0 text-[11px] text-white/60">
            <button
              className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => setDrawerOpen((v) => !v)}
              aria-expanded={drawerOpen}
            >
              {drawerOpen ? "Hide details" : "Show details"}
              {attachments.length > 0 ? <span className="text-white/50">({attachments.length} attachment{attachments.length === 1 ? "" : "s"})</span> : null}
              {props.jobError || props.runError || props.resultError ? <span className="text-rose-300">•</span> : null}
              {props.jobNotice ? <span className="text-amber-200">•</span> : null}
            </button>
          </div>

          <div className="flex items-center gap-2">
            {props.activeJobId ? (
              <button
                className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15"
                onClick={async () => {
                  const jobId = props.activeJobId;
                  if (!jobId) return;
                  try {
                    await apiCancelJob(props.effectiveBase, jobId, props.daemonAuthToken || undefined);
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
          </div>
        </div>

        <textarea
          className="mt-2 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm leading-relaxed shadow-inner max-h-[30vh] overflow-y-auto"
          data-testid="prompt"
          rows={3}
          value={props.prompt}
          placeholder="Describe the task… (Ctrl/⌘+Enter to run)"
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
            onChange={async (e) => {
              const files = Array.from(e.target.files || []);
              e.currentTarget.value = "";
              if (files.length === 0) return;
              if (uploadBusy) return;
              const sid = String(props.sessionId || "").trim() || "default";
              setUploadBusy(true);
              props.setJobNotice(null);
              try {
                const payloadFiles: { name: string; mime?: string; data_base64: string }[] = [];
                for (const f of files.slice(0, 16)) {
                  const maxBytes = 32 * 1024 * 1024;
                  if (typeof (f as any)?.size === "number" && (f as any).size > maxBytes) continue;
                  const name = String(f.name || "upload.bin");
                  const mime = String(f.type || "").trim() || guessMimeFromName(name) || undefined;
                  const data_base64 = await fileToBase64(f);
                  payloadFiles.push({ name, mime, data_base64 });
                }
                if (payloadFiles.length === 0) {
                  props.setJobNotice("no files uploaded (too large or invalid)");
                  return;
                }
                const resp = await apiPostSessionUpload(
                  props.effectiveBase,
                  { session_id: sid, files: payloadFiles },
                  props.daemonAuthToken || undefined,
                );
                if (!resp.ok) {
                  props.setJobNotice(resp.error ? `upload failed: ${resp.error}` : "upload failed");
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
            className={`inline-flex cursor-pointer items-center gap-2 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 ${uploadBusy ? "opacity-50 pointer-events-none" : ""}`}
          >
            {uploadBusy ? "Uploading…" : "Attach files"}
          </label>

          {attachments.length > 0 ? (
            <div className="text-xs text-white/60">
              Staged: <span className="text-white/80">{attachments.length}</span>
            </div>
          ) : (
            <div className="text-xs text-white/50">No staged attachments</div>
          )}
        </div>
      </div>
    </div>
  );
});

export default PromptBar;
