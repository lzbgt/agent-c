import React from "react";

import type { Attachment, PromptBarProps } from "./promptBarTypes";

type PromptBarDetailsDrawerProps = {
  attachments: Attachment[];
  chatTarget: "session" | "team";
  drawerOpen: boolean;
  onClose: () => void;
  onClearAttachments: () => void;
  onRemoveAttachment: (path: string) => void;
  teamAction: "run" | "guidance" | "goal";
} & Pick<
  PromptBarProps,
  | "activeJobId"
  | "jobError"
  | "jobNotice"
  | "jobProgressLabel"
  | "jobStatus"
  | "queueCount"
  | "resultError"
  | "runError"
  | "runWatchMode"
  | "sessionId"
  | "tools"
>;

export default function PromptBarDetailsDrawer(props: PromptBarDetailsDrawerProps) {
  if (!props.drawerOpen) return null;
  return (
    <>
      <div className="fixed inset-0 z-30" onClick={props.onClose} aria-hidden="true" />
      <div className="absolute bottom-full left-0 right-0 z-40" data-testid="promptbar-details">
        <div className="mx-auto max-w-7xl px-3 pb-2">
          <div className="max-h-[60vh] overflow-y-auto rounded-lg border border-white/10 bg-slate-950/95 shadow-xl">
            <div className="flex items-center justify-between gap-3 border-b border-white/10 px-3 py-2">
              <div className="text-xs font-semibold text-white/80">Details</div>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                type="button"
                onClick={props.onClose}
              >
                Close
              </button>
            </div>

            <div className="grid gap-3 px-3 py-3 text-xs text-white/70">
              <div className="text-[11px] text-white/60">
                session=<code className="text-white/70 break-all">{String(props.sessionId || "").trim() || "(none)"}</code>{" "}
                tools=<code className="text-white/70 break-all">{String(props.tools || "")}</code>{" "}
                run_watch=<code className="text-white/70 break-all">{String(props.runWatchMode || "local")}</code>{" "}
                target=<code className="text-white/70 break-all">{props.chatTarget}</code>{" "}
                {props.chatTarget === "team" ? (
                  <>
                    action=<code className="text-white/70 break-all">{props.teamAction}</code>{" "}
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

              <div data-testid="promptbar-attachments-section">
                <div className="flex items-center justify-between gap-2">
                  <div className="font-medium text-white/70">Attachments</div>
                  {props.attachments.length > 0 ? (
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={props.onClearAttachments}
                    >
                      Clear ({props.attachments.length})
                    </button>
                  ) : null}
                </div>

                {props.attachments.length === 0 ? (
                  <div className="mt-1 text-white/50">No attachments staged for the next run.</div>
                ) : (
                  <div className="mt-2 max-h-48 overflow-y-auto rounded-md border border-white/10 bg-black/20 p-2">
                    <div className="flex flex-wrap items-center gap-2">
                      {props.attachments.map((attachment) => (
                        <div
                          key={attachment.path}
                          className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1"
                          title={attachment.path}
                        >
                          <span className="max-w-[360px] truncate text-white/80">{attachment.name || attachment.path}</span>
                          <button
                            className="text-white/60 hover:text-white"
                            type="button"
                            onClick={() => props.onRemoveAttachment(attachment.path)}
                            aria-label={`Remove ${attachment.name || attachment.path}`}
                          >
                            ×
                          </button>
                        </div>
                      ))}
                    </div>
                  </div>
                )}

                <div className="mt-2 text-white/50">
                  Attachments apply to the next <span className="text-white/70">Run</span> only. After the run is accepted by the daemon
                  (async) or completes (sync), the staged list is cleared. Uploaded files may still remain in the session storage on the daemon.
                </div>
                {props.chatTarget === "team" ? (
                  <div className="mt-2 text-white/50">Team attachments are shared with all team members when you send.</div>
                ) : null}
              </div>

              {props.runError ? (
                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">Failed: {props.runError}</div>
              ) : null}
              {props.jobError ? (
                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">{props.jobError}</div>
              ) : null}
              {props.jobNotice ? (
                <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">{props.jobNotice}</div>
              ) : null}
              {props.resultError ? (
                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">{props.resultError}</div>
              ) : null}
            </div>
          </div>
        </div>
      </div>
    </>
  );
}
