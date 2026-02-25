import React from "react";
import {
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiCancelWorkflow,
  apiGetClientPrefs,
  apiGetWorkflow,
  apiPostClientPrefs,
  apiSubmitWorkflow,
} from "../api";
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
  authKey?: string;
  clientId?: string;
  workflowDefaults?: Record<string, any>;
  workflowTargets?: string[];
  workflowBearerEnv?: string;
  onSubmitted?: (workflowId: string) => void;
};

type TemplateKind = "llm_dag" | "agent_parallel" | "agent_parallel_demo";
type ComposerMode = "json" | "graph";
type GraphSetter = React.Dispatch<React.SetStateAction<GraphState>>;
type WaitState = {
  workflowId: string;
  status: string;
  elapsedSec: number;
  active: boolean;
  startedUnixMs: number;
};
type WaitStatePersisted = {
  workflow_id: string;
  started_unix_ms: number;
  last_status?: string;
  updated_unix_ms?: number;
};

const TEMPLATE_LABELS: Record<TemplateKind, string> = {
  llm_dag: "LLM DAG (A→B/C)",
  agent_parallel: "Agent collaboration (agentd_parallel)",
  agent_parallel_demo: "Agent collaboration demo (agentd_parallel)",
};

const WAIT_PREFS_KIND = "webui-workflow";
const WAIT_PREFS_VERSION = 1;

const pruneWaitByScope = (
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

const extractWorkflowWaitByScope = (prefs: any): Record<string, WaitStatePersisted> => {
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

const brokerBaseFromProxy = (baseUrl: string) => {
  const trimmed = String(baseUrl || "").trim().replace(/\/+$/, "");
  const marker = "/v1/agents/";
  const idx = trimmed.indexOf(marker);
  if (idx >= 0) return trimmed.slice(0, idx);
  return trimmed;
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

const buildAgentParallelDemoTemplate = (
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
  const [cancelBusy, setCancelBusy] = React.useState(false);
  const [waitState, setWaitState] = React.useState<WaitState | null>(null);
  const waitScopeKey = React.useMemo(() => {
    const base = String(props.baseUrl || "").trim();
    const key = String(props.authKey || "").trim();
    return `${base}::${key}`;
  }, [props.authKey, props.baseUrl]);
  const waitServerScopeKey = React.useMemo(() => String(props.baseUrl || "").trim(), [props.baseUrl]);
  const waitStaleMs = 7 * 24 * 60 * 60 * 1000;
  const [waitByScope, setWaitByScope] = useLocalStorageState<Record<string, WaitStatePersisted>>(
    "agentui.workflowWaitByScope",
    {},
  );
  const [serverWaitByScope, setServerWaitByScope] = React.useState<Record<string, WaitStatePersisted>>({});
  const [serverWaitStatus, setServerWaitStatus] = React.useState<"idle" | "loading" | "ready" | "error">("idle");
  const serverPrefsClientId = React.useMemo(() => String(props.clientId || "webui"), [props.clientId]);
  const serverPrefsBase = React.useMemo(() => {
    const base = String(props.baseUrl || "").trim();
    if (!base) return "";
    if (props.auth?.mode === "broker") return brokerBaseFromProxy(base);
    return base;
  }, [props.auth?.mode, props.baseUrl]);
  const serverPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: Record<string, WaitStatePersisted> | null;
  }>({ timer: null, pending: null });

  const pushServerWait = React.useCallback(
    async (nextMap: Record<string, WaitStatePersisted>) => {
      if (!serverPrefsBase || !serverPrefsClientId) return;
      const payload = {
        client_id: serverPrefsClientId,
        client_kind: WAIT_PREFS_KIND,
        prefs: { workflow_wait: { version: WAIT_PREFS_VERSION, by_scope: nextMap } },
      };
      const resp =
        props.auth?.mode === "broker"
          ? await apiBrokerPostClientPrefs(serverPrefsBase, payload, props.auth)
          : await apiPostClientPrefs(serverPrefsBase, payload, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "workflow prefs update failed");
      }
      setServerWaitStatus("ready");
    },
    [props.auth, serverPrefsBase, serverPrefsClientId],
  );

  const scheduleServerPersist = React.useCallback(
    (nextMap: Record<string, WaitStatePersisted>) => {
      if (!serverPrefsBase || !serverPrefsClientId) return;
      if (serverWaitStatus === "error") return;
      serverPersistRef.current.pending = nextMap;
      if (serverPersistRef.current.timer) return;
      serverPersistRef.current.timer = setTimeout(() => {
        const pending = serverPersistRef.current.pending;
        serverPersistRef.current.pending = null;
        serverPersistRef.current.timer = null;
        if (!pending) return;
        pushServerWait(pending).catch(() => {
          setServerWaitStatus("error");
        });
      }, 1500);
    },
    [pushServerWait, serverPrefsBase, serverPrefsClientId, serverWaitStatus],
  );

  const loadServerWait = React.useCallback(async () => {
    if (!serverPrefsBase || !serverPrefsClientId) return;
    setServerWaitStatus("loading");
    try {
      const resp =
        props.auth?.mode === "broker"
          ? await apiBrokerGetClientPrefs(serverPrefsBase, serverPrefsClientId, WAIT_PREFS_KIND, props.auth)
          : await apiGetClientPrefs(serverPrefsBase, serverPrefsClientId, WAIT_PREFS_KIND, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "workflow prefs fetch failed");
      }
      const nextMap = pruneWaitByScope(extractWorkflowWaitByScope(resp.prefs), Date.now(), waitStaleMs);
      setServerWaitByScope(nextMap);
      setServerWaitStatus("ready");
    } catch (err) {
      setServerWaitStatus("error");
    }
  }, [props.auth, serverPrefsBase, serverPrefsClientId, waitStaleMs]);

  React.useEffect(() => {
    if (!serverPrefsBase || !serverPrefsClientId) return;
    void loadServerWait();
  }, [loadServerWait, props.authKey, serverPrefsBase, serverPrefsClientId]);

  React.useEffect(
    () => () => {
      if (serverPersistRef.current.timer) {
        clearTimeout(serverPersistRef.current.timer);
      }
    },
    [],
  );

  const localPersistedEntry = React.useMemo(() => {
    const now = Date.now();
    const fresh = pruneWaitByScope(waitByScope, now, waitStaleMs);
    const rec = fresh[waitScopeKey];
    if (rec && typeof rec.workflow_id === "string" && rec.workflow_id.trim()) {
      return { key: waitScopeKey, value: rec, extra: 0 };
    }
    const basePrefix = `${waitScopeKey.split("::")[0]}::`;
    const matches = Object.entries(fresh).filter(([key, value]) => {
      if (!key.startsWith(basePrefix)) return false;
      return value && typeof value.workflow_id === "string" && value.workflow_id.trim().length > 0;
    });
    if (matches.length >= 1) {
      matches.sort((a, b) => {
        const av = a[1];
        const bv = b[1];
        const ats = typeof av.updated_unix_ms === "number" ? av.updated_unix_ms : av.started_unix_ms ?? 0;
        const bts = typeof bv.updated_unix_ms === "number" ? bv.updated_unix_ms : bv.started_unix_ms ?? 0;
        return bts - ats;
      });
      return { key: matches[0][0], value: matches[0][1], extra: Math.max(0, matches.length - 1) };
    }
    return null;
  }, [waitByScope, waitScopeKey, waitStaleMs]);

  const serverPersistedEntry = React.useMemo(() => {
    if (!waitServerScopeKey) return null;
    const now = Date.now();
    const fresh = pruneWaitByScope(serverWaitByScope, now, waitStaleMs);
    const rec = fresh[waitServerScopeKey];
    if (rec && typeof rec.workflow_id === "string" && rec.workflow_id.trim()) {
      return { key: waitServerScopeKey, value: rec, extra: Math.max(0, Object.keys(fresh).length - 1) };
    }
    return null;
  }, [serverWaitByScope, waitServerScopeKey, waitStaleMs]);

  const waitPersistedEntry = serverPersistedEntry ?? localPersistedEntry;
  const waitPersisted = waitPersistedEntry?.value ?? null;
  const waitPersistedKey = localPersistedEntry?.key ?? waitScopeKey;
  const waitPersistedExtra = waitPersistedEntry?.extra ?? 0;
  const writeWaitPersisted = React.useCallback(
    (next: WaitStatePersisted | null) => {
      setWaitByScope((prev) => {
        const out = { ...prev };
        if (waitPersistedKey && waitPersistedKey !== waitScopeKey) {
          delete out[waitPersistedKey];
        }
        if (next && next.workflow_id) {
          out[waitScopeKey] = next;
        } else {
          delete out[waitScopeKey];
        }
        return out;
      });
      if (!serverPrefsBase || !waitServerScopeKey) return;
      if (serverWaitStatus === "error") return;
      setServerWaitByScope((prev) => {
        const out = { ...prev };
        if (next && next.workflow_id) {
          out[waitServerScopeKey] = next;
        } else {
          delete out[waitServerScopeKey];
        }
        const pruned = pruneWaitByScope(out, Date.now(), waitStaleMs);
        scheduleServerPersist(pruned);
        return pruned;
      });
    },
    [
      scheduleServerPersist,
      serverPrefsBase,
      serverWaitStatus,
      setServerWaitByScope,
      setWaitByScope,
      waitPersistedKey,
      waitScopeKey,
      waitServerScopeKey,
      waitStaleMs,
    ],
  );
  const resumeAttemptedRef = React.useRef<string>("");

  React.useEffect(() => {
    const fresh = pruneWaitByScope(waitByScope, Date.now(), waitStaleMs);
    if (Object.keys(fresh).length === Object.keys(waitByScope).length) return;
    setWaitByScope(fresh);
  }, [setWaitByScope, waitByScope, waitStaleMs]);

  React.useEffect(() => {
    if (serverWaitStatus !== "ready") return;
    const fresh = pruneWaitByScope(serverWaitByScope, Date.now(), waitStaleMs);
    if (Object.keys(fresh).length === Object.keys(serverWaitByScope).length) return;
    setServerWaitByScope(fresh);
    scheduleServerPersist(fresh);
  }, [scheduleServerPersist, serverWaitByScope, serverWaitStatus, waitStaleMs]);

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

  const waitForWorkflow = React.useCallback(
    async (workflowId: string, opts?: { startedUnixMs?: number }) => {
      const startedUnixMs =
        typeof opts?.startedUnixMs === "number" && Number.isFinite(opts.startedUnixMs) ? opts.startedUnixMs : Date.now();
      let last: any = null;
      const persist = (status: string) => {
        writeWaitPersisted({
          workflow_id: workflowId,
          started_unix_ms: startedUnixMs,
          last_status: status,
          updated_unix_ms: Date.now(),
        });
      };
      persist("running");
      setWaitState({
        workflowId,
        status: "running",
        elapsedSec: Math.max(0, Math.round((Date.now() - startedUnixMs) / 1000)),
        active: true,
        startedUnixMs,
      });
      while (Date.now() - startedUnixMs < 180_000) {
        const resp = await apiGetWorkflow(
          props.baseUrl,
          {
            workflowId,
            includeTasks: true,
            includeResults: true,
          },
          props.auth,
        );
        last = resp;
        const status = String(resp?.workflow?.status || "").toLowerCase();
        const display = status || "unknown";
        setWaitState({
          workflowId,
          status: display,
          elapsedSec: Math.max(0, Math.round((Date.now() - startedUnixMs) / 1000)),
          active: status === "running" || status === "queued",
          startedUnixMs,
        });
        persist(display);
        if (status && status !== "running" && status !== "queued") {
          writeWaitPersisted(null);
          return resp;
        }
        await new Promise((resolve) => setTimeout(resolve, 5000));
      }
      setWaitState((prev) =>
        prev
          ? {
              ...prev,
              status: "timeout",
              elapsedSec: Math.max(0, Math.round((Date.now() - startedUnixMs) / 1000)),
              active: false,
              startedUnixMs,
            }
          : null,
      );
      persist("timeout");
      return last;
    },
    [props.baseUrl, props.auth, writeWaitPersisted],
  );

  const submitWorkflow = React.useCallback(
    async (payload: Record<string, any>, opts?: { wait?: boolean }) => {
      const baseUrl = String(props.baseUrl || "").trim();
      if (!baseUrl) {
        setSubmitError("Base URL is not set.");
        return;
      }
      setSubmitBusy(true);
      setSubmitError(null);
      setWaitState(null);
      setCancelBusy(false);
      try {
        const resp = await apiSubmitWorkflow(baseUrl, payload, props.auth);
        setSubmitResult(resp);
        if (resp && resp.workflow_id) {
          props.onSubmitted?.(resp.workflow_id);
          if (opts?.wait) {
            const finalResp = await waitForWorkflow(resp.workflow_id, { startedUnixMs: Date.now() });
            if (finalResp) {
              setSubmitResult(finalResp);
              if (finalResp.ok === false) {
                setSubmitError(finalResp.error || "Workflow lookup failed");
              }
            }
          }
        } else if (resp && resp.ok === false) {
          setSubmitError(resp.error || "Workflow submit failed");
        }
      } catch (err) {
        setSubmitError(String(err));
      } finally {
        setSubmitBusy(false);
        setWaitState((prev) => (prev ? { ...prev, active: false } : prev));
      }
    },
    [props.baseUrl, props.auth, props.onSubmitted, waitForWorkflow],
  );

  const cancelWorkflow = React.useCallback(async () => {
    const baseUrl = String(props.baseUrl || "").trim();
    if (!baseUrl) {
      setSubmitError("Base URL is not set.");
      return;
    }
    if (!waitState?.workflowId) {
      return;
    }
    setCancelBusy(true);
    try {
      const resp = await apiCancelWorkflow(baseUrl, waitState.workflowId, props.auth);
      if (resp && resp.ok === false) {
        setSubmitError(resp.error || "Workflow cancel failed");
      } else {
        const startedUnixMs =
          typeof waitState.startedUnixMs === "number" && Number.isFinite(waitState.startedUnixMs)
            ? waitState.startedUnixMs
            : Date.now();
        writeWaitPersisted({
          workflow_id: waitState.workflowId,
          started_unix_ms: startedUnixMs,
          last_status: "cancel_requested",
          updated_unix_ms: Date.now(),
        });
        setWaitState((prev) =>
          prev
            ? {
                ...prev,
                status: "cancel_requested",
                active: false,
              }
            : prev,
        );
      }
    } catch (err) {
      setSubmitError(String(err));
    } finally {
      setCancelBusy(false);
    }
  }, [props.baseUrl, props.auth, waitState, writeWaitPersisted]);

  const resumePersistedWait = React.useCallback(async () => {
    if (!waitPersisted) return;
    setSubmitError(null);
    setSubmitResult(null);
    const startedUnixMs =
      typeof waitPersisted.started_unix_ms === "number" && Number.isFinite(waitPersisted.started_unix_ms)
        ? waitPersisted.started_unix_ms
        : Date.now();
    const resp = await waitForWorkflow(waitPersisted.workflow_id, { startedUnixMs });
    if (resp) {
      setSubmitResult(resp);
      if (resp.ok === false) {
        setSubmitError(resp.error || "Workflow lookup failed");
      }
    }
  }, [waitPersisted, waitForWorkflow]);

  const clearPersistedWait = React.useCallback(() => {
    writeWaitPersisted(null);
    setWaitState(null);
  }, [writeWaitPersisted]);

  React.useEffect(() => {
    const persisted = waitPersisted;
    if (!persisted || !persisted.workflow_id) {
      resumeAttemptedRef.current = "";
      return;
    }
    const baseUrl = String(props.baseUrl || "").trim();
    if (!baseUrl) return;
    if (waitState?.active) return;
    if (resumeAttemptedRef.current === persisted.workflow_id) return;
    resumeAttemptedRef.current = persisted.workflow_id;
    let cancelled = false;
    const workflowId = persisted.workflow_id;
    const startedUnixMs =
      typeof persisted.started_unix_ms === "number" && Number.isFinite(persisted.started_unix_ms)
        ? persisted.started_unix_ms
        : Date.now();
    (async () => {
      try {
        const resp = await apiGetWorkflow(
          baseUrl,
          {
            workflowId,
            includeTasks: true,
            includeResults: true,
          },
          props.auth,
        );
        if (cancelled) return;
        if (resp && resp.ok === false) {
          setSubmitError(resp.error || "Workflow lookup failed");
          return;
        }
        const status = String(resp?.workflow?.status || "").toLowerCase();
        if (status === "running" || status === "queued") {
          setWaitState({
            workflowId,
            status: status || "running",
            elapsedSec: Math.max(0, Math.round((Date.now() - startedUnixMs) / 1000)),
            active: true,
            startedUnixMs,
          });
          const finalResp = await waitForWorkflow(workflowId, { startedUnixMs });
          if (cancelled) return;
          if (finalResp) {
            setSubmitResult(finalResp);
            if (finalResp.ok === false) {
              setSubmitError(finalResp.error || "Workflow lookup failed");
            }
          }
          return;
        }
        setWaitState({
          workflowId,
          status: status || "unknown",
          elapsedSec: Math.max(0, Math.round((Date.now() - startedUnixMs) / 1000)),
          active: false,
          startedUnixMs,
        });
        setSubmitResult(resp);
        writeWaitPersisted(null);
      } catch (err) {
        if (!cancelled) {
          setSubmitError(String(err));
        }
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [props.auth, props.baseUrl, waitForWorkflow, waitPersisted, waitState?.active, writeWaitPersisted]);

  React.useEffect(() => {
    if (defaults.api_key && !allowInlineKeys) {
      setAllowInlineKeys(true);
    }
  }, [allowInlineKeys, defaults.api_key, setAllowInlineKeys]);

  const applyTemplate = (kind: TemplateKind, opts?: { toGraph?: boolean }) => {
    setTemplateKind(kind);
    let template: Record<string, any>;
    if (kind === "agent_parallel_demo") {
      template = buildAgentParallelDemoTemplate(defaults, targets, bearerEnv, allowInlineKeys);
    } else if (kind === "agent_parallel") {
      template = buildAgentParallelTemplate(defaults, targets, bearerEnv, allowInlineKeys);
    } else {
      template = buildLlmDagTemplate(defaults, allowInlineKeys);
    }
    const cleaned = JSON.stringify(template, null, 2);
    setComposerJson(cleaned);
    setSubmitError(null);
    setSubmitResult(null);
    if (opts?.toGraph) {
      try {
        const parsed = parseWorkflowToGraph(cleaned);
        setGraphState(parsed.state);
        setGraphParseWarnings(parsed.warnings);
        setComposerMode("graph");
      } catch (err) {
        setSubmitError(String(err));
      }
    }
  };

  const submitDemoAndWait = async () => {
    const template = buildAgentParallelDemoTemplate(defaults, targets, bearerEnv, allowInlineKeys);
    setTemplateKind("agent_parallel_demo");
    const cleaned = JSON.stringify(template, null, 2);
    setComposerJson(cleaned);
    setSubmitError(null);
    setSubmitResult(null);
    await submitWorkflow(template, { wait: true });
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
    setComposerMode("json");
  };

  const clearGraphWarnings = () => {
    setGraphParseWarnings([]);
  };

  const submit = async () => {
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
    await submitWorkflow(payload);
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
              data-testid="workflow-composer-tab-json"
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
              data-testid="workflow-composer-tab-graph"
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
                onClick={() => applyTemplate("agent_parallel_demo", { toGraph: true })}
              >
                Demo → Graph
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => void submitDemoAndWait()}
                disabled={submitBusy}
              >
                Demo → Submit (wait)
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
          data-testid="workflow-composer-json"
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
        {serverWaitStatus === "error" ? (
          <span className="text-[11px] text-amber-200">wait sync: local-only</span>
        ) : serverWaitStatus === "loading" ? (
          <span className="text-[11px] text-white/50">wait sync: loading…</span>
        ) : serverWaitStatus === "ready" ? (
          <span className="text-[11px] text-emerald-200">wait sync: server</span>
        ) : null}
        {waitState ? (
          <>
            <span className="text-[11px] text-white/60">
              Wait: {waitState.status} • {waitState.elapsedSec}s • {waitState.workflowId}
            </span>
            {waitState.active ? (
              <button
                className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                type="button"
                onClick={() => void cancelWorkflow()}
                disabled={cancelBusy}
              >
                {cancelBusy ? "Canceling…" : "Cancel"}
              </button>
            ) : null}
          </>
        ) : waitPersisted ? (
          <>
            <span className="text-[11px] text-white/60">
              Resume wait: {waitPersisted.workflow_id}
              {waitPersisted.last_status ? ` • ${waitPersisted.last_status}` : ""}
              {waitPersistedExtra > 0 ? ` • +${waitPersistedExtra} more` : ""}
            </span>
            <button
              className="rounded-md border border-emerald-400/30 bg-emerald-400/10 px-2 py-1 text-[11px] text-emerald-100 hover:bg-emerald-400/20"
              type="button"
              onClick={() => void resumePersistedWait()}
            >
              Resume
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={clearPersistedWait}
            >
              Clear
            </button>
          </>
        ) : null}
      </div>

      {submitResult ? (
        <pre className="mt-2 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
          {JSON.stringify(submitResult, null, 2)}
        </pre>
      ) : null}
    </div>
  );
}
