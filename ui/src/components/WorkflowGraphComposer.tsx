import React from "react";

import { DEFAULT_GRAPH_STATE, normalizeTargets, type GraphNode, type GraphNodeKind, withAutoLayout } from "../workflowGraph";
import WorkflowGraphCanvas from "./workflow/WorkflowGraphCanvas";
import WorkflowGraphInspector from "./workflow/WorkflowGraphInspector";
import WorkflowGraphToolbar from "./workflow/WorkflowGraphToolbar";
import {
  WORKFLOW_GRAPH_NODE_HEIGHT,
  WORKFLOW_GRAPH_NODE_WIDTH,
  type WorkflowGraphComposerProps,
} from "./workflow/workflowGraphComposerTypes";

const nodePromptDefault = (kind: GraphNodeKind) =>
  kind === "agent_parallel" ? "Remote agent: propose an approach for the goal." : "Draft a short plan for the goal.";

const ensureUniqueId = (existing: Set<string>, base: string, currentId?: string): string => {
  const trimmed = base.trim();
  if (!trimmed) return currentId || "T";
  if (!existing.has(trimmed) || trimmed === currentId) return trimmed;
  let i = 2;
  while (existing.has(`${trimmed}_${i}`)) i += 1;
  return `${trimmed}_${i}`;
};

const clamp = (value: number, min: number, max: number) => Math.min(Math.max(value, min), max);

export default function WorkflowGraphComposer({
  state,
  onChange,
  buildResult,
  buildError,
  parseWarnings,
  onImportJson,
  onExportJson,
  bearerEnv,
  onClearWarnings,
}: WorkflowGraphComposerProps) {
  const containerRef = React.useRef<HTMLDivElement | null>(null);
  const [selectedId, setSelectedId] = React.useState<string | null>(null);
  const [connectingFrom, setConnectingFrom] = React.useState<string | null>(null);
  const [dragging, setDragging] = React.useState<{
    id: string;
    startX: number;
    startY: number;
    originX: number;
    originY: number;
  } | null>(null);

  const nodes = Array.isArray(state.nodes) ? state.nodes : [];
  const edges = Array.isArray(state.edges) ? state.edges : [];
  const nodeMap = React.useMemo(() => new Map(nodes.map((node) => [node.id, node])), [nodes]);
  const selectedNode = selectedId ? nodeMap.get(selectedId) : undefined;

  React.useEffect(() => {
    if (selectedId && !nodeMap.has(selectedId)) setSelectedId(null);
  }, [nodeMap, selectedId]);

  React.useEffect(() => {
    if (!dragging) return;
    const handleMove = (event: PointerEvent) => {
      const rect = containerRef.current?.getBoundingClientRect();
      const maxX = rect ? rect.width - WORKFLOW_GRAPH_NODE_WIDTH : 2000;
      const maxY = rect ? rect.height - WORKFLOW_GRAPH_NODE_HEIGHT : 2000;
      const dx = event.clientX - dragging.startX;
      const dy = event.clientY - dragging.startY;
      const nextX = clamp(dragging.originX + dx, 0, maxX);
      const nextY = clamp(dragging.originY + dy, 0, maxY);
      onChange((prev) => ({
        ...prev,
        nodes: prev.nodes.map((node) => (node.id === dragging.id ? { ...node, x: nextX, y: nextY } : node)),
      }));
    };
    const handleUp = () => setDragging(null);
    window.addEventListener("pointermove", handleMove);
    window.addEventListener("pointerup", handleUp);
    return () => {
      window.removeEventListener("pointermove", handleMove);
      window.removeEventListener("pointerup", handleUp);
    };
  }, [dragging, onChange]);

  const addNode = (kind: GraphNodeKind) => {
    onChange((prev) => {
      const ids = new Set(prev.nodes.map((node) => node.id));
      const base = kind === "agent_parallel" ? "R" : "N";
      const id = ensureUniqueId(ids, base);
      const offset = prev.nodes.length * 20;
      const node: GraphNode = {
        id,
        kind,
        x: 40 + offset,
        y: 40 + offset,
        prompt: nodePromptDefault(kind),
        targets: kind === "agent_parallel" ? [] : undefined,
      };
      setSelectedId(id);
      return { ...prev, nodes: [...prev.nodes, node] };
    });
  };

  const removeNode = (id: string) => {
    onChange((prev) => ({
      ...prev,
      nodes: prev.nodes.filter((node) => node.id !== id),
      edges: prev.edges.filter((edge) => edge.from !== id && edge.to !== id),
    }));
    if (selectedId === id) setSelectedId(null);
  };

  const updateNode = (id: string, patch: Partial<GraphNode>) => {
    onChange((prev) => ({
      ...prev,
      nodes: prev.nodes.map((node) => (node.id === id ? { ...node, ...patch } : node)),
    }));
  };

  const renameNode = (id: string, nextIdRaw: string) => {
    onChange((prev) => {
      const ids = new Set(prev.nodes.map((node) => node.id));
      const nextId = ensureUniqueId(ids, nextIdRaw, id);
      setSelectedId(nextId);
      return {
        ...prev,
        nodes: prev.nodes.map((node) => (node.id === id ? { ...node, id: nextId } : node)),
        edges: prev.edges.map((edge) => ({
          from: edge.from === id ? nextId : edge.from,
          to: edge.to === id ? nextId : edge.to,
        })),
      };
    });
  };

  const addEdge = (from: string, to: string) => {
    if (from === to) return;
    onChange((prev) => {
      if (prev.edges.some((edge) => edge.from === from && edge.to === to)) return prev;
      return { ...prev, edges: [...prev.edges, { from, to }] };
    });
  };

  const removeEdge = (from: string, to: string) => {
    onChange((prev) => ({ ...prev, edges: prev.edges.filter((edge) => !(edge.from === from && edge.to === to)) }));
  };

  const handleConnectStart = (id: string) => {
    setConnectingFrom((prev) => (prev === id ? null : id));
  };

  const handleConnectEnd = (id: string) => {
    if (!connectingFrom) return;
    addEdge(connectingFrom, id);
    setConnectingFrom(null);
  };

  const resetGraph = () => {
    onChange(DEFAULT_GRAPH_STATE);
    setSelectedId(DEFAULT_GRAPH_STATE.nodes[0]?.id || null);
  };

  return (
    <div className="mt-3 grid gap-2 text-xs text-white/70">
      <WorkflowGraphToolbar
        parseWarnings={parseWarnings}
        buildWarnings={buildResult?.warnings ?? []}
        buildError={buildError}
        hasRemoteNodes={nodes.some((node) => node.kind === "agent_parallel")}
        bearerEnv={bearerEnv}
        connectingFrom={connectingFrom}
        onAddNode={addNode}
        onAutoLayout={() => onChange((prev) => withAutoLayout(prev))}
        onImportJson={onImportJson}
        onExportJson={onExportJson}
        onResetGraph={resetGraph}
        onClearWarnings={onClearWarnings}
        onCancelConnect={() => setConnectingFrom(null)}
      />

      <div className="grid gap-2 lg:grid-cols-[1fr_240px]">
        <WorkflowGraphCanvas
          containerRef={containerRef}
          nodes={nodes.map((node) => (node.kind === "agent_parallel" ? { ...node, targets: normalizeTargets(node.targets) } : node))}
          edges={edges}
          nodeMap={nodeMap}
          selectedId={selectedId}
          connectingFrom={connectingFrom}
          onSelectNode={setSelectedId}
          onStartDrag={(id, startX, startY, originX, originY) => setDragging({ id, startX, startY, originX, originY })}
          onConnectStart={handleConnectStart}
          onConnectEnd={handleConnectEnd}
          onCanvasPointerDown={() => {
            setSelectedId(null);
            setConnectingFrom(null);
          }}
        />
        <WorkflowGraphInspector
          selectedNode={selectedNode}
          edges={edges}
          onRenameNode={renameNode}
          onUpdateNode={updateNode}
          onRemoveEdge={removeEdge}
          onRemoveNode={removeNode}
        />
      </div>
    </div>
  );
}
