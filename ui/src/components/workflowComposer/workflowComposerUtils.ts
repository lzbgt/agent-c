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

export const extractWorkflowWaitByScope = (prefs: any): Record<string, WaitStatePersisted> => {
  if (!prefs || typeof prefs !== "object") return {};
  const raw = prefs.workflow_wait;
  if (!raw || typeof raw !== "object") return {};
  const byScope = raw.by_scope;
  if (!byScope || typeof byScope !== "object") return {};
  const out: Record<string, WaitStatePersisted> = {};
  for (const [key, value] of Object.entries(byScope)) {
    if (!value || typeof value !== "object") continue;
    const workflowId = typeof (value as any).workflow_id === "string" ? String((value as any).workflow_id).trim() : "";
    if (!workflowId) continue;
    out[key] = value as WaitStatePersisted;
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

export const buildLlmDagTemplate = (defaults: Record<string, any>, allowInlineKeys: boolean) => {
  const workflow: Record<string, any> = {
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
  defaults: Record<string, any>,
  targets: string[],
  bearerEnv: string | undefined,
  allowInlineKeys: boolean,
  opts?: { timeoutMs?: number; pollMs?: number; inputGoal?: string },
) => {
  const normTargets = normalizeTargets(targets);
  const targetEntries = normTargets.length
    ? normTargets.map((base, idx) => ({ id: `a${idx + 1}`, base_url: base }))
    : [{ id: "a1", base_url: "http://127.0.0.1:8123" }];
  const inputGoal = opts?.inputGoal;
  const workflow: Record<string, any> = {
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
              defaults: isNonEmptyObject(defaults) ? defaults : undefined,
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
            ok_pointer: "/ok",
            value_pointer: "/agentd/final/result/results_by_task/RUN/assistant_text",
          },
        },
      },
    ],
  };
  if (allowInlineKeys && defaults.api_key) {
    workflow.allow_inline_api_keys = true;
    if (workflow.tasks?.[0]?.agentd_parallel?.agentd_call?.workflow) {
      workflow.tasks[0].agentd_parallel.agentd_call.workflow.allow_inline_api_keys = true;
    }
  }
  return workflow;
};

export const buildAgentParallelDemoTemplate = (
  defaults: Record<string, any>,
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
  defaults: Record<string, any>,
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
