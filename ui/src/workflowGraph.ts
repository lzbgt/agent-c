export type GraphNodeKind = "llm" | "agent_parallel";

export type GraphNode = {
  id: string;
  kind: GraphNodeKind;
  x: number;
  y: number;
  prompt: string;
  targets?: string[];
};

export type GraphEdge = {
  from: string;
  to: string;
};

export type GraphState = {
  nodes: GraphNode[];
  edges: GraphEdge[];
};

export type GraphBuildOptions = {
  defaults: Record<string, any>;
  allowInlineKeys: boolean;
  targets: string[];
  bearerEnv?: string;
};

export type GraphBuildResult = {
  workflow: Record<string, any>;
  warnings: string[];
};

export type GraphImportResult = {
  state: GraphState;
  warnings: string[];
};

const DEFAULT_PROMPT = "Draft a short plan for the goal.";

export const DEFAULT_GRAPH_STATE: GraphState = {
  nodes: [
    {
      id: "A",
      kind: "llm",
      x: 60,
      y: 60,
      prompt: DEFAULT_PROMPT,
    },
  ],
  edges: [],
};

export const isNonEmptyObject = (v: any) => v && typeof v === "object" && !Array.isArray(v) && Object.keys(v).length > 0;

export const normalizeTargets = (targets?: string[]): string[] => {
  if (!Array.isArray(targets)) return [];
  return targets.map((t) => String(t || "").trim()).filter((t) => t.length > 0);
};

const ensureUniqueId = (ids: Set<string>, baseId: string): string => {
  let candidate = baseId.trim() || "T";
  if (!ids.has(candidate)) return candidate;
  let i = 2;
  while (ids.has(`${candidate}_${i}`)) i += 1;
  return `${candidate}_${i}`;
};

const clampNumber = (value: number, min: number, max: number) => Math.min(Math.max(value, min), max);

const layoutGraph = (nodes: GraphNode[], edges: GraphEdge[]): GraphNode[] => {
  const ids = new Set(nodes.map((n) => n.id));
  const depsMap = new Map<string, string[]>();
  for (const n of nodes) depsMap.set(n.id, []);
  for (const edge of edges) {
    if (!ids.has(edge.from) || !ids.has(edge.to)) continue;
    const deps = depsMap.get(edge.to);
    if (deps) deps.push(edge.from);
  }

  const levels = new Map<string, number>();
  const visiting = new Set<string>();
  const computeLevel = (id: string): number => {
    if (levels.has(id)) return levels.get(id) ?? 0;
    if (visiting.has(id)) return 0;
    visiting.add(id);
    const deps = depsMap.get(id) ?? [];
    let maxDep = -1;
    for (const dep of deps) maxDep = Math.max(maxDep, computeLevel(dep));
    visiting.delete(id);
    const level = maxDep + 1;
    levels.set(id, level);
    return level;
  };

  nodes.forEach((n) => computeLevel(n.id));

  const buckets = new Map<number, GraphNode[]>();
  nodes.forEach((n) => {
    const lvl = levels.get(n.id) ?? 0;
    const arr = buckets.get(lvl) ?? [];
    arr.push(n);
    buckets.set(lvl, arr);
  });

  const orderedLevels = Array.from(buckets.keys()).sort((a, b) => a - b);
  const xSpacing = 240;
  const ySpacing = 140;
  const startX = 40;
  const startY = 40;
  const out: GraphNode[] = [];

  for (const lvl of orderedLevels) {
    const row = buckets.get(lvl) ?? [];
    row.sort((a, b) => a.id.localeCompare(b.id));
    row.forEach((node, idx) => {
      out.push({
        ...node,
        x: startX + lvl * xSpacing,
        y: startY + idx * ySpacing,
      });
    });
  }

  return out;
};

const buildAgentParallelTask = (
  node: GraphNode,
  options: GraphBuildOptions,
  warnings: string[],
): Record<string, any> => {
  const nodeTargets = normalizeTargets(node.targets);
  let targets = nodeTargets.length ? nodeTargets : normalizeTargets(options.targets);
  if (!targets.length) {
    warnings.push(`Node ${node.id}: no targets configured; using placeholder base_url.`);
    targets = ["http://127.0.0.1:8123"];
  }
  const targetEntries = targets.map((base, idx) => ({ id: `t${idx + 1}`, base_url: base }));

  const nestedWorkflow: Record<string, any> = {
    tasks: [
      {
        task_id: "RUN",
        request: {
          prompt: node.prompt || DEFAULT_PROMPT,
          no_session: true,
        },
      },
    ],
  };
  if (isNonEmptyObject(options.defaults)) nestedWorkflow.defaults = options.defaults;
  if (options.allowInlineKeys && options.defaults.api_key) nestedWorkflow.allow_inline_api_keys = true;

  return {
    task_id: node.id,
    kind: "agentd_parallel",
    agentd_parallel: {
      targets: targetEntries,
      agentd_call: {
        op: "workflow_submit_and_wait",
        timeout_ms: 20000,
        poll_ms: 50,
        include_results: true,
        include_tasks: false,
        ...(options.bearerEnv ? { bearer_env: options.bearerEnv } : {}),
        workflow: nestedWorkflow,
      },
      aggregate: {
        mode: "first_ok",
        ok_pointer: "/ok",
        value_pointer: "/agentd/final/result/results_by_task/RUN/assistant_text",
      },
    },
  };
};

export const buildWorkflowFromGraph = (state: GraphState, options: GraphBuildOptions): GraphBuildResult => {
  if (!state || !Array.isArray(state.nodes) || !state.nodes.length) {
    throw new Error("Graph has no nodes.");
  }

  const warnings: string[] = [];
  const ids = new Set<string>();
  const cleanedNodes: GraphNode[] = [];
  for (const node of state.nodes) {
    const id = String(node.id || "").trim();
    if (!id) {
      warnings.push("Found node with empty id; skipping.");
      continue;
    }
    const uniqueId = ensureUniqueId(ids, id);
    if (uniqueId !== id) warnings.push(`Renamed duplicate node id '${id}' to '${uniqueId}'.`);
    ids.add(uniqueId);
    cleanedNodes.push({ ...node, id: uniqueId });
  }
  if (!cleanedNodes.length) throw new Error("Graph has no valid nodes.");

  const edges: GraphEdge[] = Array.isArray(state.edges)
    ? state.edges.filter((e) => ids.has(e.from) && ids.has(e.to) && e.from !== e.to)
    : [];

  const depsMap = new Map<string, string[]>();
  cleanedNodes.forEach((n) => depsMap.set(n.id, []));
  for (const edge of edges) {
    const deps = depsMap.get(edge.to);
    if (!deps) continue;
    if (!deps.includes(edge.from)) deps.push(edge.from);
  }

  const tasks = cleanedNodes
    .slice()
    .sort((a, b) => a.id.localeCompare(b.id))
    .map((node) => {
      const deps = depsMap.get(node.id) ?? [];
      const baseTask: Record<string, any> = { task_id: node.id };
      if (deps.length) baseTask.depends_on = deps;
      if (node.kind === "agent_parallel") {
        return { ...baseTask, ...buildAgentParallelTask(node, options, warnings) };
      }
      return {
        ...baseTask,
        request: {
          prompt: node.prompt || DEFAULT_PROMPT,
          no_session: true,
        },
      };
    });

  const workflow: Record<string, any> = { tasks };
  if (isNonEmptyObject(options.defaults)) workflow.defaults = options.defaults;
  if (options.allowInlineKeys && options.defaults.api_key) workflow.allow_inline_api_keys = true;

  return { workflow, warnings };
};

const parseAgentParallelPrompt = (task: any): string | null => {
  const wf = task?.agentd_parallel?.agentd_call?.workflow;
  if (!wf || !Array.isArray(wf.tasks)) return null;
  for (const t of wf.tasks) {
    const prompt = t?.request?.prompt;
    if (typeof prompt === "string" && prompt.trim()) return prompt.trim();
  }
  return null;
};

const parseAgentParallelTargets = (task: any, warnings: string[]): string[] => {
  const targets = task?.agentd_parallel?.targets;
  if (!Array.isArray(targets)) return [];
  const out: string[] = [];
  for (const t of targets) {
    if (t && typeof t.base_url === "string" && t.base_url.trim()) {
      out.push(t.base_url.trim());
      continue;
    }
    const broker = t?.broker_proxy;
    if (broker && typeof broker === "object") {
      const brokerBase = String(broker.broker_base_url || "").trim();
      const agentId = String(broker.agent_id || "").trim();
      if (brokerBase && agentId) {
        const base = brokerBase.replace(/\/$/, "") + `/v1/agents/${agentId}/proxy`;
        out.push(base);
        warnings.push(`Converted broker_proxy target to base_url for ${agentId}.`);
        continue;
      }
    }
  }
  return out;
};

export const parseWorkflowToGraph = (jsonText: string): GraphImportResult => {
  const warnings: string[] = [];
  let parsed: any;
  try {
    parsed = JSON.parse(jsonText || "{}");
  } catch (err) {
    throw new Error(`Invalid JSON: ${String(err)}`);
  }
  const tasks = Array.isArray(parsed?.tasks) ? parsed.tasks : [];
  if (!tasks.length) {
    throw new Error("No tasks found in workflow JSON.");
  }

  const nodes: GraphNode[] = [];
  const edges: GraphEdge[] = [];

  for (const task of tasks) {
    const taskId = String(task?.task_id || "").trim();
    if (!taskId) {
      warnings.push("Skipped task with missing task_id.");
      continue;
    }
    const dependsOn = Array.isArray(task?.depends_on)
      ? task.depends_on.map((d: any) => String(d || "").trim()).filter(Boolean)
      : [];
    for (const dep of dependsOn) edges.push({ from: dep, to: taskId });

    const prompt = typeof task?.request?.prompt === "string" ? task.request.prompt.trim() : "";
    if (prompt) {
      nodes.push({ id: taskId, kind: "llm", x: 0, y: 0, prompt });
      continue;
    }

    if (String(task?.kind || "") === "agentd_parallel") {
      const remotePrompt = parseAgentParallelPrompt(task) || DEFAULT_PROMPT;
      const targets = parseAgentParallelTargets(task, warnings);
      nodes.push({ id: taskId, kind: "agent_parallel", x: 0, y: 0, prompt: remotePrompt, targets });
      continue;
    }

    warnings.push(`Unsupported task '${taskId}' ignored (kind: ${String(task?.kind || "") || "request"}).`);
  }

  if (!nodes.length) {
    throw new Error("No supported tasks found in workflow JSON.");
  }

  const layoutNodes = layoutGraph(nodes, edges);

  return { state: { nodes: layoutNodes, edges }, warnings };
};

export const withAutoLayout = (state: GraphState): GraphState => {
  if (!state || !Array.isArray(state.nodes)) return state;
  const nodes = layoutGraph(state.nodes, Array.isArray(state.edges) ? state.edges : []);
  return { ...state, nodes };
};

export const clampGraphState = (state: GraphState): GraphState => {
  const nodes = Array.isArray(state.nodes) ? state.nodes : [];
  const edges = Array.isArray(state.edges) ? state.edges : [];
  const cleanedNodes: GraphNode[] = nodes.map((node): GraphNode => ({
    ...node,
    id: String(node.id || "").trim(),
    kind: node.kind === "agent_parallel" ? "agent_parallel" : "llm",
    x: clampNumber(Number(node.x || 0), -2000, 2000),
    y: clampNumber(Number(node.y || 0), -2000, 2000),
    prompt: String(node.prompt || ""),
    targets: Array.isArray(node.targets) ? node.targets.map((t) => String(t || "")) : undefined,
  }));
  return { nodes: cleanedNodes, edges };
};
