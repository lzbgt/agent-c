import React from "react";

import { TEMPLATE_LABELS } from "./workflowComposerUtils";
import type { ComposerMode, TemplateKind } from "./workflowComposerTypes";

type WorkflowComposerToolbarProps = {
  composerMode: ComposerMode;
  templateKind: TemplateKind;
  submitBusy: boolean;
  onSetComposerMode: (next: ComposerMode) => void;
  onApplyTemplate: (kind: TemplateKind, opts?: { toGraph?: boolean }) => void;
  onSubmitDemoAndWait: () => Promise<void>;
  onFormatJson: () => void;
};

export default function WorkflowComposerToolbar(props: WorkflowComposerToolbarProps) {
  return (
    <div className="flex flex-wrap items-center justify-between gap-2">
      <div className="text-xs font-semibold text-white/70">Workflow composer</div>
      <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <div className="flex items-center gap-1">
          <button
            type="button"
            className={`rounded-md border px-2 py-1 text-[11px] ${
              props.composerMode === "json"
                ? "border-sky-400/60 bg-sky-400/10 text-sky-100"
                : "border-white/10 bg-black/30 text-white/60 hover:bg-black/40"
            }`}
            data-testid="workflow-composer-tab-json"
            onClick={() => props.onSetComposerMode("json")}
          >
            JSON
          </button>
          <button
            type="button"
            className={`rounded-md border px-2 py-1 text-[11px] ${
              props.composerMode === "graph"
                ? "border-sky-400/60 bg-sky-400/10 text-sky-100"
                : "border-white/10 bg-black/30 text-white/60 hover:bg-black/40"
            }`}
            data-testid="workflow-composer-tab-graph"
            onClick={() => props.onSetComposerMode("graph")}
          >
            Graph
          </button>
        </div>
        {props.composerMode === "json" ? (
          <>
            <label className="flex items-center gap-1">
              template
              <select
                className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                value={props.templateKind}
                onChange={(event) => props.onApplyTemplate(event.target.value as TemplateKind)}
              >
                {Object.entries(TEMPLATE_LABELS).map(([key, label]) => (
                  <option key={key} value={key}>
                    {label}
                  </option>
                ))}
              </select>
            </label>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => props.onApplyTemplate(props.templateKind)}
            >
              Apply
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => props.onApplyTemplate("agent_parallel_demo", { toGraph: true })}
            >
              Demo → Graph
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void props.onSubmitDemoAndWait()}
              disabled={props.submitBusy}
            >
              Demo → Submit (wait)
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={props.onFormatJson}
            >
              Format
            </button>
          </>
        ) : null}
      </div>
    </div>
  );
}
