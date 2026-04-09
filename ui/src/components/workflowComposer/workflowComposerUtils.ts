import {
  DEFAULT_GRAPH_STATE,
  buildWorkflowFromGraph,
  clampGraphState,
  isNonEmptyObject,
  normalizeTargets,
  parseWorkflowToGraph,
  type GraphBuildResult,
  type GraphState,
} from "../../workflowGraph";
import type { WorkflowDefaults, WorkflowSpec } from "../../workflowTypes";
import type {
  TemplateKind,
  WaitStatePersisted,
  WorkflowComposerGraphBuild,
} from "./workflowComposerTypes";

export { DEFAULT_GRAPH_STATE, clampGraphState, normalizeTargets, parseWorkflowToGraph };
export type { GraphBuildResult, GraphState };

export const TEMPLATE_LABELS: Record<TemplateKind, string> = {
  llm_dag: "LLM DAG (A→B/C)",
  agent_parallel: "Agent collaboration (agentd_parallel)",
  agent_parallel_demo: "Agent collaboration demo (agentd_parallel)",
};

export const WAIT_PREFS_KIND = "webui-workflow";
export const WAIT_PREFS_VERSION = 1;

const isObjectRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === "object" && !Array.isArray(value);

const normalizeWaitStatePersisted = (value: unknown): WaitStatePersisted | null => {
  if (!isObjectRecord(value)) return null;
  const workflowId = typeof value.workflow_id === "string" ? value.workflow_id.trim() : "";
  if (!workflowId) return null;
  const startedUnixMs = typeof value.started_unix_ms === "number" ? value.started_unix_ms : NaN;
  if (!Number.isFinite(startedUnixMs)) return null;
  return {
    workflow_id: workflowId,
    started_unix_ms: startedUnixMs,
    last_status: typeof value.last_status === "string" ? value.last_status : undefined,
    updated_unix_ms: typeof value.updated_unix_ms === "number" ? value.updated_unix_ms : undefined,
  };
};

export const pruneWaitByScope = (
  input: Record<string, WaitStatePersisted> | null | undefined,
  now: number,
  staleMs: number,
) => {
  const out: Record<string, WaitStatePersisted> = {};
  if (!input || typeof input !== "object") return out;
  for (const [key, value] of Object.entries(input)) {
    if (!value || typeof value !== "object") continue;
    const ts =
      typeof value.updated_unix_ms === "number"
        ? value.updated_unix_ms
        : typeof value.started_unix_ms === "number"
          ? value.started_unix_ms
          : 0;
    if (!ts || now - ts <= staleMs) {
      out[key] = value;
    }
  }
  return out;
};

export const extractWorkflowWaitByScope = (prefs: unknown): Record<string, WaitStatePersisted> => {
  if (!isObjectRecord(prefs)) return {};
  const raw = isObjectRecord(prefs.workflow_wait) ? prefs.workflow_wait : null;
  if (!raw) return {};
  const byScope = isObjectRecord(raw.by_scope) ? raw.by_scope : null;
  if (!byScope) return {};
  const out: Record<string, WaitStatePersisted> = {};
  for (const [key, value] of Object.entries(byScope)) {
    const normalized = normalizeWaitStatePersisted(value);
    if (normalized) out[key] = normalized;
  }
  return out;
};

export const waitStateTs = (value?: WaitStatePersisted | null) => {
  if (!value) return 0;
  if (typeof value.updated_unix_ms === "number") return value.updated_unix_ms;
  if (typeof value.started_unix_ms === "number") return value.started_unix_ms;
  return 0;
};

export const mergeWaitMaps = (
  primary: Record<string, WaitStatePersisted>,
  secondary: Record<string, WaitStatePersisted>,
) => {
  const out: Record<string, WaitStatePersisted> = { ...secondary };
  for (const [key, value] of Object.entries(primary)) {
    const existing = out[key];
    if (!existing || waitStateTs(value) >= waitStateTs(existing)) {
      out[key] = value;
    }
  }
  return out;
};

export const waitMapsEqual = (a: Record<string, WaitStatePersisted>, b: Record<string, WaitStatePersisted>) => {
  const keysA = Object.keys(a);
  const keysB = Object.keys(b);
  if (keysA.length !== keysB.length) return false;
  for (const key of keysA) {
    const av = a[key];
    const bv = b[key];
    if (!bv) return false;
    if (av.workflow_id !== bv.workflow_id) return false;
    if (waitStateTs(av) !== waitStateTs(bv)) return false;
    if ((av.last_status || "") !== (bv.last_status || "")) return false;
  }
  return true;
};

export const buildLlmDagTemplate = (defaults: WorkflowDefaults, allowInlineKeys: boolean): WorkflowSpec => {
  const workflow: WorkflowSpec = {
    tasks: [
      {
        task_id: "A",
        request: {
          prompt: "Agent A: draft a short plan for the goal.",
          no_session: true,
        },
      },
      {
        task_id: "B",
        depends_on: ["A"],
        request: {
          prompt: "Agent B: critique the plan from A and suggest improvements. Plan: ${task.A.assistant_text}",
          no_session: true,
        },
      },
      {
        task_id: "C",
        depends_on: ["A"],
        request: {
          prompt: "Agent C: list risks and edge cases for the plan. Plan: ${task.A.assistant_text}",
          no_session: true,
        },
      },
    ],
  };
  if (isNonEmptyObject(defaults)) {
    workflow.defaults = defaults;
  }
  if (allowInlineKeys && defaults.api_key) {
    workflow.allow_inline_api_keys = true;
  }
  return workflow;
};

export const buildAgentParallelTemplate = (
  defaults: WorkflowDefaults,
  targets: string[],
  bearerEnv: string | undefined,
  allowInlineKeys: boolean,
  opts?: { timeoutMs?: number; pollMs?: number; inputGoal?: string },
): WorkflowSpec => {
  const normTargets = normalizeTargets(targets);
  const targetEntries = normTargets.length
    ? normTargets.map((base, idx) => ({ id: `a${idx + 1}`, base_url: base }))
    : [{ id: "a1", base_url: "http://127.0.0.1:8123" }];
  const inputGoal = opts?.inputGoal;
  const workflow: WorkflowSpec = {
    ...(inputGoal ? { inputs: { goal: inputGoal } } : {}),
    tasks: [
      {
        task_id: "COLLAB",
        kind: "agentd_parallel",
        agentd_parallel: {
          targets: targetEntries,
          agentd_call: {
            op: "workflow_submit_and_wait",
            timeout_ms: opts?.timeoutMs ?? 20000,
            poll_ms: opts?.pollMs ?? 50,
            include_results: true,
            include_tasks: false,
            ...(bearerEnv ? { bearer_env: bearerEnv } : {}),
            workflow: {
              ...(isNonEmptyObject(defaults) ? { defaults } : {}),
              tasks: [
                {
                  task_id: "RUN",
                  request: {
                    prompt: inputGoal
                      ? "Remote agent: propose an approach for the goal: ${input.goal}"
                      : "Remote agent: propose an approach for the goal.",
                    no_session: true,
                  },
                },
              ],
            },
          },
          aggregate: {
            mode: "first_ok",
            task_ids: targetEntries.map((target) => `COLLAB:${target.id}`),
            ok_pointer: "/ok",
            value_pointer: "/agentd/final/result/results_by_task/RUN/assistant_text",
          },
        },
      },
    ],
  };
  if (allowInlineKeys && defaults.api_key) {
    workflow.allow_inline_api_keys = true;
    const firstTask = workflow.tasks[0];
    if (firstTask && "kind" in firstTask && firstTask.kind === "agentd_parallel") {
      firstTask.agentd_parallel.agentd_call.workflow.allow_inline_api_keys = true;
    }
  }
  return workflow;
};

export const buildAgentParallelDemoTemplate = (
  defaults: WorkflowDefaults,
  targets: string[],
  bearerEnv: string | undefined,
  allowInlineKeys: boolean,
) =>
  buildAgentParallelTemplate(defaults, targets, bearerEnv, allowInlineKeys, {
    timeoutMs: 120000,
    pollMs: 200,
    inputGoal: "Draft a collaborative plan for a multi-agent workflow graph demo.",
  });

export const buildGraphWorkflow = (
  graphState: GraphState,
  defaults: WorkflowDefaults,
  allowInlineKeys: boolean,
  targets: string[],
  bearerEnv: string | undefined,
): WorkflowComposerGraphBuild => {
  try {
    const result = buildWorkflowFromGraph(graphState, {
      defaults,
      allowInlineKeys,
      targets,
      bearerEnv,
    });
    return { result, error: null };
  } catch (err) {
    return { result: null, error: String(err) };
  }
};
