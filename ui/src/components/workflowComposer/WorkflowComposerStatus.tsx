import React from "react";

import type { WaitState, WaitStatePersisted } from "./workflowComposerTypes";

type WorkflowComposerStatusProps = {
  cancelBusy: boolean;
  serverWaitStatus: "idle" | "loading" | "ready" | "error";
  submitBusy: boolean;
  submitError: string | null;
  submitResult: any | null;
  waitPersisted: WaitStatePersisted | null;
  waitPersistedExtra: number;
  waitState: WaitState | null;
  onCancelWorkflow: () => Promise<void>;
  onClearPersistedWait: () => void;
  onResumePersistedWait: () => Promise<void>;
  onSubmit: () => Promise<void>;
};

export default function WorkflowComposerStatus(props: WorkflowComposerStatusProps) {
  return (
    <>
      <div className="mt-2 flex flex-wrap items-center gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => void props.onSubmit()}
          disabled={props.submitBusy}
        >
          {props.submitBusy ? "Submitting…" : "Submit workflow"}
        </button>
        {props.submitError ? <span className="text-xs text-rose-200">{props.submitError}</span> : null}
        {props.serverWaitStatus === "error" ? (
          <span className="text-[11px] text-amber-200">wait sync: local-only</span>
        ) : props.serverWaitStatus === "loading" ? (
          <span className="text-[11px] text-white/50">wait sync: loading…</span>
        ) : props.serverWaitStatus === "ready" ? (
          <span className="text-[11px] text-emerald-200">wait sync: server</span>
        ) : null}
        {props.waitState ? (
          <>
            <span className="text-[11px] text-white/60">
              Wait: {props.waitState.status} • {props.waitState.elapsedSec}s • {props.waitState.workflowId}
            </span>
            {props.waitState.active ? (
              <button
                className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                type="button"
                onClick={() => void props.onCancelWorkflow()}
                disabled={props.cancelBusy}
              >
                {props.cancelBusy ? "Canceling…" : "Cancel"}
              </button>
            ) : null}
          </>
        ) : props.waitPersisted ? (
          <>
            <span className="text-[11px] text-white/60">
              Resume wait: {props.waitPersisted.workflow_id}
              {props.waitPersisted.last_status ? ` • ${props.waitPersisted.last_status}` : ""}
              {props.waitPersistedExtra > 0 ? ` • +${props.waitPersistedExtra} more` : ""}
            </span>
            <button
              className="rounded-md border border-emerald-400/30 bg-emerald-400/10 px-2 py-1 text-[11px] text-emerald-100 hover:bg-emerald-400/20"
              type="button"
              onClick={() => void props.onResumePersistedWait()}
            >
              Resume
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={props.onClearPersistedWait}
            >
              Clear
            </button>
          </>
        ) : null}
      </div>

      {props.submitResult ? (
        <pre className="mt-2 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
          {JSON.stringify(props.submitResult, null, 2)}
        </pre>
      ) : null}
    </>
  );
}
