import React from "react";

import {
  WORKFLOW_GRAPH_NODE_HEADER_HEIGHT,
  WORKFLOW_GRAPH_NODE_HEIGHT,
  WORKFLOW_GRAPH_NODE_WIDTH,
  type WorkflowGraphCanvasProps,
} from "./workflowGraphComposerTypes";

export default function WorkflowGraphCanvas({
  containerRef,
  nodes,
  edges,
  nodeMap,
  selectedId,
  connectingFrom,
  onSelectNode,
  onStartDrag,
  onConnectStart,
  onConnectEnd,
  onCanvasPointerDown,
}: WorkflowGraphCanvasProps) {
  return (
    <div
      ref={containerRef as React.RefObject<HTMLDivElement>}
      className="relative h-80 overflow-hidden rounded-md border border-white/10 bg-black/40"
      data-testid="workflow-graph-canvas"
      style={{
        backgroundImage: "radial-gradient(circle at 1px 1px, rgba(255,255,255,0.08) 1px, transparent 0)",
        backgroundSize: "18px 18px",
      }}
      onPointerDown={() => {
        onCanvasPointerDown();
      }}
    >
      <svg className="absolute inset-0 h-full w-full" style={{ pointerEvents: "none" }}>
        {edges.map((edge) => {
          const from = nodeMap.get(edge.from);
          const to = nodeMap.get(edge.to);
          if (!from || !to) return null;
          const x1 = from.x + WORKFLOW_GRAPH_NODE_WIDTH;
          const y1 = from.y + WORKFLOW_GRAPH_NODE_HEADER_HEIGHT;
          const x2 = to.x;
          const y2 = to.y + WORKFLOW_GRAPH_NODE_HEADER_HEIGHT;
          const mid = (x2 - x1) * 0.5;
          const path = `M ${x1} ${y1} C ${x1 + mid} ${y1}, ${x2 - mid} ${y2}, ${x2} ${y2}`;
          return (
            <path key={`${edge.from}-${edge.to}`} d={path} stroke="rgba(255,255,255,0.35)" strokeWidth="1.5" fill="none" />
          );
        })}
      </svg>

      {nodes.map((node) => (
        <div
          key={node.id}
          className={`absolute rounded-md border bg-black/60 text-[11px] text-white/80 shadow-sm ${
            selectedId === node.id ? "border-sky-400/70" : "border-white/10"
          }`}
          data-testid={`workflow-graph-node-${node.id}`}
          style={{
            width: WORKFLOW_GRAPH_NODE_WIDTH,
            height: WORKFLOW_GRAPH_NODE_HEIGHT,
            left: node.x,
            top: node.y,
          }}
          onPointerDown={(e) => {
            e.stopPropagation();
            onSelectNode(node.id);
          }}
        >
          <div
            className="flex cursor-grab items-center justify-between gap-2 border-b border-white/10 bg-white/5 px-2 py-1"
            onPointerDown={(e) => {
              e.stopPropagation();
              onStartDrag(node.id, e.clientX, e.clientY, node.x, node.y);
            }}
          >
            <span className="font-mono text-[10px] text-white/70">{node.id}</span>
            <span className="text-[10px] text-white/50">{node.kind === "agent_parallel" ? "remote" : "llm"}</span>
          </div>
          <div className="px-2 py-1 text-[10px] text-white/70">
            <div className="max-h-[48px] overflow-hidden">{node.prompt || "(empty prompt)"}</div>
            {node.kind === "agent_parallel" ? (
              <div className="mt-1 text-[10px] text-white/40">targets: {Array.isArray(node.targets) ? node.targets.length || "default" : "default"}</div>
            ) : null}
          </div>
          <button
            type="button"
            className={`absolute -right-2 top-1/2 h-3 w-3 -translate-y-1/2 rounded-full border ${
              connectingFrom === node.id ? "border-sky-300 bg-sky-300/40" : "border-white/40 bg-black/60"
            }`}
            data-testid={`workflow-graph-handle-from-${node.id}`}
            onClick={(e) => {
              e.stopPropagation();
              onConnectStart(node.id);
            }}
            title="Connect from"
          />
          <button
            type="button"
            className="absolute -left-2 top-1/2 h-3 w-3 -translate-y-1/2 rounded-full border border-white/40 bg-black/60"
            data-testid={`workflow-graph-handle-to-${node.id}`}
            onClick={(e) => {
              e.stopPropagation();
              onConnectEnd(node.id);
            }}
            title="Connect to"
          />
        </div>
      ))}
    </div>
  );
}
