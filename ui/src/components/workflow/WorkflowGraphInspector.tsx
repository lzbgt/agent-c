import React from "react";

import { normalizeTargets, type GraphNodeKind } from "../../workflowGraph";
import type { WorkflowGraphInspectorProps } from "./workflowGraphComposerTypes";

export default function WorkflowGraphInspector({
  selectedNode,
  edges,
  onRenameNode,
  onUpdateNode,
  onRemoveEdge,
  onRemoveNode,
}: WorkflowGraphInspectorProps) {
  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-2" data-testid="workflow-graph-inspector">
      <div className="text-[11px] font-semibold text-white/70">Node inspector</div>
      {selectedNode ? (
        <div className="mt-2 grid gap-2 text-[11px] text-white/70">
          <label className="grid gap-1">
            <span className="text-[10px] text-white/50">task_id</span>
            <input
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
              data-testid="workflow-graph-node-id"
              value={selectedNode.id}
              onChange={(e) => onRenameNode(selectedNode.id, e.target.value)}
            />
          </label>
          <label className="grid gap-1">
            <span className="text-[10px] text-white/50">kind</span>
            <select
              className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
              value={selectedNode.kind}
              onChange={(e) => onUpdateNode(selectedNode.id, { kind: e.target.value as GraphNodeKind })}
            >
              <option value="llm">llm</option>
              <option value="agent_parallel">agent_parallel</option>
            </select>
          </label>
          <label className="grid gap-1">
            <span className="text-[10px] text-white/50">prompt</span>
            <textarea
              className="min-h-[80px] rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
              value={selectedNode.prompt}
              onChange={(e) => onUpdateNode(selectedNode.id, { prompt: e.target.value })}
            />
          </label>
          {selectedNode.kind === "agent_parallel" ? (
            <label className="grid gap-1">
              <span className="text-[10px] text-white/50">targets (comma or newline separated)</span>
              <textarea
                className="min-h-[60px] rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
                value={(selectedNode.targets || []).join("\n")}
                onChange={(e) => onUpdateNode(selectedNode.id, { targets: normalizeTargets(e.target.value.split(/[,\n]/)) })}
              />
            </label>
          ) : null}
          <div className="text-[10px] text-white/50">depends_on</div>
          <div className="space-y-1">
            {edges
              .filter((edge) => edge.to === selectedNode.id)
              .map((edge) => (
                <div key={`${edge.from}-${edge.to}`} className="flex items-center justify-between gap-2">
                  <span className="font-mono text-[10px] text-white/70">{edge.from}</span>
                  <button
                    type="button"
                    className="text-[10px] text-rose-200"
                    onClick={() => onRemoveEdge(edge.from, edge.to)}
                  >
                    remove
                  </button>
                </div>
              ))}
            {edges.filter((edge) => edge.to === selectedNode.id).length === 0 ? (
              <div className="text-[10px] text-white/40">Use connect handles to add dependencies.</div>
            ) : null}
          </div>
          <button
            type="button"
            className="mt-2 rounded-md border border-rose-500/40 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200"
            onClick={() => onRemoveNode(selectedNode.id)}
          >
            Remove node
          </button>
        </div>
      ) : (
        <div className="mt-2 text-[11px] text-white/40">Select a node to edit.</div>
      )}
    </div>
  );
}
