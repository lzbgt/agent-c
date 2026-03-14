import React from "react";

import type { WorkflowGraphToolbarProps } from "./workflowGraphComposerTypes";

export default function WorkflowGraphToolbar({
  parseWarnings,
  buildWarnings,
  buildError,
  hasRemoteNodes,
  bearerEnv,
  connectingFrom,
  onAddNode,
  onAutoLayout,
  onImportJson,
  onExportJson,
  onResetGraph,
  onClearWarnings,
  onCancelConnect,
}: WorkflowGraphToolbarProps) {
  const warnings = [...parseWarnings, ...buildWarnings];

  return (
    <>
      <div className="flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-add-llm"
          onClick={() => onAddNode("llm")}
        >
          Add LLM node
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-add-remote"
          onClick={() => onAddNode("agent_parallel")}
        >
          Add remote node
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-autolayout"
          onClick={onAutoLayout}
        >
          Auto-layout
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-import-json"
          onClick={onImportJson}
        >
          Import JSON
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-export-json"
          onClick={onExportJson}
        >
          Export JSON
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          data-testid="workflow-graph-reset"
          onClick={onResetGraph}
        >
          Reset
        </button>
      </div>

      {warnings.length > 0 ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          <div className="flex items-center justify-between">
            <span>Graph warnings</span>
            <button className="text-[10px] text-amber-200" type="button" onClick={onClearWarnings}>
              clear
            </button>
          </div>
          <ul className="mt-1 list-disc space-y-1 pl-4">
            {warnings.map((warning) => (
              <li key={warning}>{warning}</li>
            ))}
          </ul>
        </div>
      ) : null}

      {buildError ? <div className="text-rose-200">{buildError}</div> : null}

      {hasRemoteNodes ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          agentd_parallel tasks require `--workflow-enable-http-tasks` on the primary agentd.
        </div>
      ) : null}

      {!bearerEnv && hasRemoteNodes ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          No bearer env configured. Remote agent targets that require auth may fail.
        </div>
      ) : null}

      <div className="text-[11px] text-white/50">
        Connect tasks: click the right handle on a node, then click the left handle on the dependent node.
      </div>
      {connectingFrom ? (
        <div className="flex items-center gap-2 text-[11px] text-sky-100">
          Connecting from <span className="font-mono">{connectingFrom}</span>
          <button
            type="button"
            className="rounded-md border border-sky-400/40 bg-sky-400/10 px-2 py-0.5 text-[10px] text-sky-100"
            onClick={onCancelConnect}
          >
            Cancel
          </button>
        </div>
      ) : null}
    </>
  );
}
