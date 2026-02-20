import React from "react";
import { apiSubmitWorkflow } from "../api";
import type { ApiAuth } from "../api/auth";
import useLocalStorageState from "../hooks/useLocalStorageState";
import WorkflowGraphComposer from "./WorkflowGraphComposer";
import {
  DEFAULT_GRAPH_STATE,
  buildWorkflowFromGraph,
  clampGraphState,
  isNonEmptyObject,
  normalizeTargets,
  parseWorkflowToGraph,
  type GraphBuildResult,
  type GraphState,
} from "../workflowGraph";

export type WorkflowComposerProps = {
  baseUrl: string;
  auth?: ApiAuth;
  workflowDefaults?: Record<string, any>;
  workflowTargets?: string[];
  workflowBearerEnv?: string;
  onSubmitted?: (workflowId: string) => void;
};

type TemplateKind = "llm_dag" | "agent_parallel";
type ComposerMode = "json" | "graph";
type GraphSetter = React.Dispatch<React.SetStateAction<GraphState>>;

const TEMPLATE_LABELS: Record<TemplateKind, string> = {
  llm_dag: "LLM DAG (A→B/C)",
  agent_parallel: "Agent collaboration (agentd_parallel)",
};

const buildLlmDagTemplate = (defaults: Record<string, any>, allowInlineKeys: boolean) => {
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

const buildAgentParallelTemplate = (
  defaults: Record<string, any>,
  targets: string[],
  bearerEnv: string | undefined,
  allowInlineKeys: boolean,
) => {
  const normTargets = normalizeTargets(targets);
  const targetEntries = normTargets.length
    ? normTargets.map((base, idx) => ({ id: `a${idx + 1}`, base_url: base }))
    : [{ id: "a1", base_url: "http://127.0.0.1:8123" }];
  const workflow: Record<string, any> = {
    tasks: [
      {
        task_id: "COLLAB",
        kind: "agentd_parallel",
        agentd_parallel: {
          targets: targetEntries,
          agentd_call: {
            op: "workflow_submit_and_wait",
            timeout_ms: 20000,
            poll_ms: 50,
            include_results: true,
            include_tasks: false,
            ...(bearerEnv ? { bearer_env: bearerEnv } : {}),
            workflow: {
              defaults: isNonEmptyObject(defaults) ? defaults : undefined,
              tasks: [
                {
                  task_id: "RUN",
                  request: {
                    prompt: "Remote agent: propose an approach for the goal.",
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

export default function WorkflowComposer(props: WorkflowComposerProps) {
  const [templateKind, setTemplateKind] = useLocalStorageState<TemplateKind>(
    "agentui.workflowComposerTemplate",
    "llm_dag",
  );
  const [composerJson, setComposerJson] = useLocalStorageState("agentui.workflowComposerJson", "");
  const [allowInlineKeys, setAllowInlineKeys] = useLocalStorageState(
    "agentui.workflowComposerAllowInlineKeys",
    false,
  );
  const [composerMode, setComposerMode] = useLocalStorageState<ComposerMode>(
    "agentui.workflowComposerMode",
    "json",
  );
  const [graphStateRaw, setGraphStateRaw] = useLocalStorageState<GraphState>(
    "agentui.workflowComposerGraph",
    DEFAULT_GRAPH_STATE,
  );
  const [graphParseWarnings, setGraphParseWarnings] = React.useState<string[]>([]);
  const [submitError, setSubmitError] = React.useState<string | null>(null);
  const [submitResult, setSubmitResult] = React.useState<any | null>(null);
  const [submitBusy, setSubmitBusy] = React.useState(false);

  const defaults = React.useMemo(() => props.workflowDefaults ?? {}, [props.workflowDefaults]);
  const targets = React.useMemo(() => normalizeTargets(props.workflowTargets), [props.workflowTargets]);
  const bearerEnv = React.useMemo(() => (props.workflowBearerEnv || "").trim() || undefined, [props.workflowBearerEnv]);
  const graphState = React.useMemo(() => clampGraphState(graphStateRaw), [graphStateRaw]);
  const setGraphState = React.useCallback<GraphSetter>(
    (next) =>
      setGraphStateRaw((prev) => {
        const candidate = typeof next === "function" ? (next as (p: GraphState) => GraphState)(prev) : next;
        return clampGraphState(candidate);
      }),
    [setGraphStateRaw],
  );
  const graphBuild = React.useMemo<{ result: GraphBuildResult | null; error: string | null }>(() => {
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
  }, [graphState, defaults, allowInlineKeys, targets, bearerEnv]);

  React.useEffect(() => {
    if (defaults.api_key && !allowInlineKeys) {
      setAllowInlineKeys(true);
    }
  }, [allowInlineKeys, defaults.api_key, setAllowInlineKeys]);

  const applyTemplate = (kind: TemplateKind) => {
    setTemplateKind(kind);
    const template =
      kind === "agent_parallel"
        ? buildAgentParallelTemplate(defaults, targets, bearerEnv, allowInlineKeys)
        : buildLlmDagTemplate(defaults, allowInlineKeys);
    const cleaned = JSON.stringify(template, null, 2);
    setComposerJson(cleaned);
    setSubmitError(null);
    setSubmitResult(null);
  };

  const formatJson = () => {
    try {
      const parsed = JSON.parse(composerJson || "{}");
      setComposerJson(JSON.stringify(parsed, null, 2));
      setSubmitError(null);
    } catch (err) {
      setSubmitError(`Invalid JSON: ${String(err)}`);
    }
  };

  const importGraphFromJson = () => {
    try {
      const parsed = parseWorkflowToGraph(composerJson || "{}");
      setGraphState(parsed.state);
      setGraphParseWarnings(parsed.warnings);
      setSubmitError(null);
    } catch (err) {
      setSubmitError(String(err));
    }
  };

  const exportGraphToJson = () => {
    if (!graphBuild.result) {
      setSubmitError(graphBuild.error || "Graph is invalid.");
      return;
    }
    setComposerJson(JSON.stringify(graphBuild.result.workflow, null, 2));
    setSubmitError(null);
  };

  const clearGraphWarnings = () => {
    setGraphParseWarnings([]);
  };

  const submit = async () => {
    const baseUrl = String(props.baseUrl || "").trim();
    if (!baseUrl) {
      setSubmitError("Base URL is not set.");
      return;
    }
    let payload: Record<string, any>;
    if (composerMode === "graph") {
      if (!graphBuild.result) {
        setSubmitError(graphBuild.error || "Graph is invalid.");
        return;
      }
      payload = graphBuild.result.workflow;
      setComposerJson(JSON.stringify(payload, null, 2));
    } else {
      try {
        payload = JSON.parse(composerJson || "{}");
      } catch (err) {
        setSubmitError(`Invalid JSON: ${String(err)}`);
        return;
      }
    }
    setSubmitBusy(true);
    setSubmitError(null);
    try {
      const resp = await apiSubmitWorkflow(baseUrl, payload, props.auth);
      setSubmitResult(resp);
      if (resp && resp.workflow_id) {
        props.onSubmitted?.(resp.workflow_id);
      }
      if (resp && resp.ok === false) {
        setSubmitError(resp.error || "Workflow submit failed");
      }
    } catch (err) {
      setSubmitError(String(err));
    } finally {
      setSubmitBusy(false);
    }
  };

  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/70">Workflow composer</div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <div className="flex items-center gap-1">
            <button
              type="button"
              className={`rounded-md border px-2 py-1 text-[11px] ${
                composerMode === "json"
                  ? "border-sky-400/60 bg-sky-400/10 text-sky-100"
                  : "border-white/10 bg-black/30 text-white/60 hover:bg-black/40"
              }`}
              onClick={() => setComposerMode("json")}
            >
              JSON
            </button>
            <button
              type="button"
              className={`rounded-md border px-2 py-1 text-[11px] ${
                composerMode === "graph"
                  ? "border-sky-400/60 bg-sky-400/10 text-sky-100"
                  : "border-white/10 bg-black/30 text-white/60 hover:bg-black/40"
              }`}
              onClick={() => setComposerMode("graph")}
            >
              Graph
            </button>
          </div>
          {composerMode === "json" ? (
            <>
              <label className="flex items-center gap-1">
                template
                <select
                  className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={templateKind}
                  onChange={(e) => applyTemplate(e.target.value as TemplateKind)}
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
                onClick={() => applyTemplate(templateKind)}
              >
                Apply
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={formatJson}
              >
                Format
              </button>
            </>
          ) : null}
        </div>
      </div>

      <div className="mt-2 grid gap-2 text-[11px] text-white/60">
        {composerMode === "json" ? (
          <div>
            Templates are read-only helpers. Edit the JSON below before submitting.
            {templateKind === "agent_parallel" ? (
              <span className="text-amber-200"> Requires `--workflow-enable-http-tasks` on the primary agentd.</span>
            ) : null}
          </div>
        ) : (
          <div>Graph editor supports LLM and agentd_parallel tasks. Use JSON mode for advanced workflow kinds.</div>
        )}
        {defaults.api_key ? (
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={allowInlineKeys}
              onChange={(e) => setAllowInlineKeys(e.target.checked)}
            />
            allow inline API keys (stored in DB)
          </label>
        ) : null}
        {composerMode === "json" && templateKind === "agent_parallel" && !bearerEnv ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            No bearer env configured. If remote agents require auth, set a bearer env in UI runtime config
            (e.g. <code className="text-amber-100">workflowBearerEnv</code>).
          </div>
        ) : null}
      </div>

      {composerMode === "graph" ? (
        <WorkflowGraphComposer
          state={graphState}
          onChange={setGraphState}
          buildResult={graphBuild.result}
          buildError={graphBuild.error}
          parseWarnings={graphParseWarnings}
          onImportJson={importGraphFromJson}
          onExportJson={exportGraphToJson}
          bearerEnv={bearerEnv}
          onClearWarnings={clearGraphWarnings}
        />
      ) : (
        <textarea
          className="mt-3 h-64 w-full rounded-md border border-white/10 bg-black/40 p-2 font-mono text-[11px] text-white/80"
          value={composerJson}
          onChange={(e) => setComposerJson(e.target.value)}
          placeholder='Paste workflow JSON here. Use "Apply" to load a template.'
        />
      )}

      <div className="mt-2 flex flex-wrap items-center gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={submit}
          disabled={submitBusy}
        >
          {submitBusy ? "Submitting…" : "Submit workflow"}
        </button>
        {submitError ? <span className="text-xs text-rose-200">{submitError}</span> : null}
      </div>

      {submitResult ? (
        <pre className="mt-2 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
          {JSON.stringify(submitResult, null, 2)}
        </pre>
      ) : null}
    </div>
  );
}
