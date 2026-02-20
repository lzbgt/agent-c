import React from "react";
import {
  DEFAULT_GRAPH_STATE,
  type GraphBuildResult,
  type GraphNode,
  type GraphNodeKind,
  type GraphState,
  normalizeTargets,
  withAutoLayout,
} from "../workflowGraph";

const NODE_WIDTH = 190;
const NODE_HEIGHT = 120;
const NODE_HEADER_HEIGHT = 26;

type WorkflowGraphComposerProps = {
  state: GraphState;
  onChange: React.Dispatch<React.SetStateAction<GraphState>>;
  buildResult: GraphBuildResult | null;
  buildError?: string | null;
  parseWarnings: string[];
  onImportJson: () => void;
  onExportJson: () => void;
  bearerEnv?: string;
  onClearWarnings: () => void;
};

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

export default function WorkflowGraphComposer(props: WorkflowGraphComposerProps) {
  const { state, onChange, buildResult, buildError, parseWarnings, onImportJson, onExportJson, bearerEnv } = props;
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
  const nodeMap = React.useMemo(() => new Map(nodes.map((n) => [n.id, n])), [nodes]);
  const selectedNode = selectedId ? nodeMap.get(selectedId) : undefined;

  React.useEffect(() => {
    if (selectedId && !nodeMap.has(selectedId)) setSelectedId(null);
  }, [nodeMap, selectedId]);

  React.useEffect(() => {
    if (!dragging) return;
    const handleMove = (event: PointerEvent) => {
      const rect = containerRef.current?.getBoundingClientRect();
      const minX = 0;
      const minY = 0;
      const maxX = rect ? rect.width - NODE_WIDTH : 2000;
      const maxY = rect ? rect.height - NODE_HEIGHT : 2000;
      const dx = event.clientX - dragging.startX;
      const dy = event.clientY - dragging.startY;
      const nextX = clamp(dragging.originX + dx, minX, maxX);
      const nextY = clamp(dragging.originY + dy, minY, maxY);
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
      const ids = new Set(prev.nodes.map((n) => n.id));
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
    onChange((prev) => {
      const nextNodes = prev.nodes.filter((n) => n.id !== id);
      const nextEdges = prev.edges.filter((e) => e.from !== id && e.to !== id);
      return { ...prev, nodes: nextNodes, edges: nextEdges };
    });
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
      const ids = new Set(prev.nodes.map((n) => n.id));
      const nextId = ensureUniqueId(ids, nextIdRaw, id);
      const nextNodes = prev.nodes.map((node) => (node.id === id ? { ...node, id: nextId } : node));
      const nextEdges = prev.edges.map((edge) => ({
        from: edge.from === id ? nextId : edge.from,
        to: edge.to === id ? nextId : edge.to,
      }));
      setSelectedId(nextId);
      return { ...prev, nodes: nextNodes, edges: nextEdges };
    });
  };

  const addEdge = (from: string, to: string) => {
    if (from === to) return;
    onChange((prev) => {
      const exists = prev.edges.some((e) => e.from === from && e.to === to);
      if (exists) return prev;
      return { ...prev, edges: [...prev.edges, { from, to }] };
    });
  };

  const removeEdge = (from: string, to: string) => {
    onChange((prev) => ({ ...prev, edges: prev.edges.filter((e) => !(e.from === from && e.to === to)) }));
  };

  const handleConnectStart = (id: string) => {
    setConnectingFrom(id);
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

  const autoLayout = () => {
    onChange((prev) => withAutoLayout(prev));
  };

  const warnings = [...parseWarnings, ...(buildResult?.warnings ?? [])];

  return (
    <div className="mt-3 grid gap-2 text-xs text-white/70">
      <div className="flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={() => addNode("llm")}
        >
          Add LLM node
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={() => addNode("agent_parallel")}
        >
          Add remote node
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={autoLayout}
        >
          Auto-layout
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={onImportJson}
        >
          Import JSON
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={onExportJson}
        >
          Export JSON
        </button>
        <button
          type="button"
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          onClick={resetGraph}
        >
          Reset
        </button>
      </div>

      {warnings.length > 0 ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          <div className="flex items-center justify-between">
            <span>Graph warnings</span>
            <button className="text-[10px] text-amber-200" type="button" onClick={props.onClearWarnings}>
              clear
            </button>
          </div>
          <ul className="mt-1 list-disc space-y-1 pl-4">
            {warnings.map((w) => (
              <li key={w}>{w}</li>
            ))}
          </ul>
        </div>
      ) : null}

      {buildError ? <div className="text-rose-200">{buildError}</div> : null}

      {nodes.some((n) => n.kind === "agent_parallel") ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          agentd_parallel tasks require `--workflow-enable-http-tasks` on the primary agentd.
        </div>
      ) : null}

      {!bearerEnv && nodes.some((n) => n.kind === "agent_parallel") ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          No bearer env configured. Remote agent targets that require auth may fail.
        </div>
      ) : null}

      <div className="grid gap-2 lg:grid-cols-[1fr_240px]">
        <div
          ref={containerRef}
          className="relative h-80 overflow-hidden rounded-md border border-white/10 bg-black/40"
          style={{
            backgroundImage:
              "radial-gradient(circle at 1px 1px, rgba(255,255,255,0.08) 1px, transparent 0)",
            backgroundSize: "18px 18px",
          }}
          onPointerDown={() => {
            setSelectedId(null);
            setConnectingFrom(null);
          }}
        >
          <svg className="absolute inset-0 h-full w-full" style={{ pointerEvents: "none" }}>
            {edges.map((edge) => {
              const from = nodeMap.get(edge.from);
              const to = nodeMap.get(edge.to);
              if (!from || !to) return null;
              const x1 = from.x + NODE_WIDTH;
              const y1 = from.y + NODE_HEADER_HEIGHT;
              const x2 = to.x;
              const y2 = to.y + NODE_HEADER_HEIGHT;
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
              style={{
                width: NODE_WIDTH,
                height: NODE_HEIGHT,
                left: node.x,
                top: node.y,
              }}
              onPointerDown={(e) => {
                e.stopPropagation();
                setSelectedId(node.id);
              }}
            >
              <div
                className="flex cursor-grab items-center justify-between gap-2 border-b border-white/10 bg-white/5 px-2 py-1"
                onPointerDown={(e) => {
                  e.stopPropagation();
                  setDragging({ id: node.id, startX: e.clientX, startY: e.clientY, originX: node.x, originY: node.y });
                }}
              >
                <span className="font-mono text-[10px] text-white/70">{node.id}</span>
                <span className="text-[10px] text-white/50">{node.kind === "agent_parallel" ? "remote" : "llm"}</span>
              </div>
              <div className="px-2 py-1 text-[10px] text-white/70">
                <div className="max-h-[48px] overflow-hidden">{node.prompt || "(empty prompt)"}</div>
                {node.kind === "agent_parallel" ? (
                  <div className="mt-1 text-[10px] text-white/40">
                    targets: {normalizeTargets(node.targets).length || "default"}
                  </div>
                ) : null}
              </div>
              <button
                type="button"
                className={`absolute -right-2 top-1/2 h-3 w-3 -translate-y-1/2 rounded-full border ${
                  connectingFrom === node.id ? "border-sky-300 bg-sky-300/40" : "border-white/40 bg-black/60"
                }`}
                onClick={(e) => {
                  e.stopPropagation();
                  handleConnectStart(node.id);
                }}
                title="Connect from"
              />
              <button
                type="button"
                className="absolute -left-2 top-1/2 h-3 w-3 -translate-y-1/2 rounded-full border border-white/40 bg-black/60"
                onClick={(e) => {
                  e.stopPropagation();
                  handleConnectEnd(node.id);
                }}
                title="Connect to"
              />
            </div>
          ))}
        </div>

        <div className="rounded-md border border-white/10 bg-black/30 p-2">
          <div className="text-[11px] font-semibold text-white/70">Node inspector</div>
          {selectedNode ? (
            <div className="mt-2 grid gap-2 text-[11px] text-white/70">
              <label className="grid gap-1">
                <span className="text-[10px] text-white/50">task_id</span>
                <input
                  className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
                  value={selectedNode.id}
                  onChange={(e) => renameNode(selectedNode.id, e.target.value)}
                />
              </label>
              <label className="grid gap-1">
                <span className="text-[10px] text-white/50">kind</span>
                <select
                  className="rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
                  value={selectedNode.kind}
                  onChange={(e) => updateNode(selectedNode.id, { kind: e.target.value as GraphNodeKind })}
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
                  onChange={(e) => updateNode(selectedNode.id, { prompt: e.target.value })}
                />
              </label>
              {selectedNode.kind === "agent_parallel" ? (
                <label className="grid gap-1">
                  <span className="text-[10px] text-white/50">targets (comma or newline separated)</span>
                  <textarea
                    className="min-h-[60px] rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/80"
                    value={(selectedNode.targets || []).join("\n")}
                    onChange={(e) =>
                      updateNode(selectedNode.id, { targets: normalizeTargets(e.target.value.split(/[,\n]/)) })
                    }
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
                        onClick={() => removeEdge(edge.from, edge.to)}
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
                onClick={() => removeNode(selectedNode.id)}
              >
                Remove node
              </button>
            </div>
          ) : (
            <div className="mt-2 text-[11px] text-white/40">Select a node to edit.</div>
          )}
        </div>
      </div>
    </div>
  );
}
