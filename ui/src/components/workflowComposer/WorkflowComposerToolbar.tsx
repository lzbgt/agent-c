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
    <div className="grid min-w-0 gap-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <h2 className="text-sm font-semibold text-white/85">Create a workflow</h2>
        <div className="flex gap-1" role="group" aria-label="Authoring mode">
          {(["json", "graph"] as const).map((mode) => (
            <button key={mode} type="button" className="workspace-button" aria-pressed={props.composerMode === mode}
              data-testid={`workflow-composer-tab-${mode}`} onClick={() => props.onSetComposerMode(mode)}>
              {mode === "json" ? "JSON" : "Graph"}
            </button>
          ))}
        </div>
      </div>
      {props.composerMode === "json" ? (
        <>
          <label className="block min-w-0 text-xs text-white/60">
            Template
            <select aria-label="Workflow template" className="mt-2 block w-full rounded-lg border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/85"
              value={props.templateKind} onChange={(event) => props.onApplyTemplate(event.target.value as TemplateKind)}>
              {Object.entries(TEMPLATE_LABELS).map(([key, label]) => <option key={key} value={key}>{label}</option>)}
            </select>
          </label>
          <div className="flex flex-wrap items-center gap-2">
            <button className="workspace-button" type="button" onClick={() => props.onApplyTemplate(props.templateKind)}>Apply</button>
            <button className="workspace-button" type="button" onClick={props.onFormatJson}>Format</button>
            <details className="text-xs text-white/60">
              <summary className="flex cursor-pointer items-center rounded-lg border border-white/10 px-3 py-2">Parallel agent examples</summary>
              <div className="mt-2 flex flex-wrap gap-2">
                <button className="workspace-button" type="button" onClick={() => props.onApplyTemplate("agent_parallel_demo", { toGraph: true })}>Demo → Graph</button>
                <button className="workspace-button" type="button" disabled={props.submitBusy} onClick={() => void props.onSubmitDemoAndWait()}>Demo → Submit (wait)</button>
              </div>
            </details>
          </div>
        </>
      ) : null}
    </div>
  );
}
