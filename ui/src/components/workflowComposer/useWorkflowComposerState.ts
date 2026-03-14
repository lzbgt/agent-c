import React from "react";

import {
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiCancelWorkflow,
  apiGetClientPrefs,
  apiGetWorkflow,
  apiPostClientPrefs,
  apiSubmitWorkflow,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import { brokerBaseFromProxy } from "../../utils/brokerBase";
import type { WorkflowComposerProps, TemplateKind, ComposerMode, GraphSetter, WaitState, WaitStatePersisted } from "./workflowComposerTypes";
import {
  DEFAULT_GRAPH_STATE,
  WAIT_PREFS_KIND,
  WAIT_PREFS_VERSION,
  buildAgentParallelDemoTemplate,
  buildAgentParallelTemplate,
  buildGraphWorkflow,
  buildLlmDagTemplate,
  clampGraphState,
  extractWorkflowWaitByScope,
  mergeWaitMaps,
  normalizeTargets,
  parseWorkflowToGraph,
  pruneWaitByScope,
  waitMapsEqual,
} from "./workflowComposerUtils";

export default function useWorkflowComposerState(props: WorkflowComposerProps) {
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
  const [graphStateRaw, setGraphStateRaw] = useLocalStorageState("agentui.workflowComposerGraph", DEFAULT_GRAPH_STATE);
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
  const waitByScopeRef = React.useRef(waitByScope);
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
  const serverWaitLoadKeyRef = React.useRef<string>("");
  const resumeAttemptedRef = React.useRef<string>("");

  const defaults = React.useMemo(() => props.workflowDefaults ?? {}, [props.workflowDefaults]);
  const targets = React.useMemo(() => normalizeTargets(props.workflowTargets), [props.workflowTargets]);
  const bearerEnv = React.useMemo(() => (props.workflowBearerEnv || "").trim() || undefined, [props.workflowBearerEnv]);
  const graphState = React.useMemo(() => clampGraphState(graphStateRaw), [graphStateRaw]);
  const setGraphState = React.useCallback<GraphSetter>(
    (next) =>
      setGraphStateRaw((prev) => {
        const candidate = typeof next === "function" ? (next as (p: typeof prev) => typeof prev)(prev) : next;
        return clampGraphState(candidate);
      }),
    [setGraphStateRaw],
  );
  const graphBuild = React.useMemo(
    () => buildGraphWorkflow(graphState, defaults, allowInlineKeys, targets, bearerEnv),
    [allowInlineKeys, bearerEnv, defaults, graphState, targets],
  );

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
      const now = Date.now();
      const remoteMap = pruneWaitByScope(extractWorkflowWaitByScope(resp.prefs), now, waitStaleMs);
      const localMap = pruneWaitByScope(waitByScopeRef.current, now, waitStaleMs);
      const merged = mergeWaitMaps(localMap, remoteMap);
      setServerWaitByScope(merged);
      setServerWaitStatus("ready");
      if (!waitMapsEqual(merged, remoteMap)) {
        scheduleServerPersist(merged);
      }
    } catch {
      setServerWaitStatus("error");
    }
  }, [props.auth, scheduleServerPersist, serverPrefsBase, serverPrefsClientId, waitStaleMs]);
  const loadServerWaitRef = React.useRef(loadServerWait);

  React.useEffect(() => {
    loadServerWaitRef.current = loadServerWait;
  }, [loadServerWait]);

  React.useEffect(() => {
    if (!serverPrefsBase || !serverPrefsClientId) return;
    const nextKey = `${serverPrefsBase}::${serverPrefsClientId}::${String(props.authKey || "").trim()}`;
    if (serverWaitLoadKeyRef.current === nextKey) return;
    serverWaitLoadKeyRef.current = nextKey;
    void loadServerWaitRef.current();
  }, [props.authKey, serverPrefsBase, serverPrefsClientId]);

  React.useEffect(() => {
    if (serverPrefsBase && serverPrefsClientId) return;
    serverWaitLoadKeyRef.current = "";
  }, [serverPrefsBase, serverPrefsClientId]);

  React.useEffect(() => {
    waitByScopeRef.current = waitByScope;
  }, [waitByScope]);

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
      setWaitByScope,
      waitPersistedKey,
      waitScopeKey,
      waitServerScopeKey,
      waitStaleMs,
    ],
  );

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
    [props.auth, props.baseUrl, writeWaitPersisted],
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
    [props.auth, props.baseUrl, props.onSubmitted, waitForWorkflow],
  );

  const cancelWorkflow = React.useCallback(async () => {
    const baseUrl = String(props.baseUrl || "").trim();
    if (!baseUrl) {
      setSubmitError("Base URL is not set.");
      return;
    }
    if (!waitState?.workflowId) return;
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
  }, [props.auth, props.baseUrl, waitState, writeWaitPersisted]);

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
  }, [waitForWorkflow, waitPersisted]);

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

  const applyTemplate = React.useCallback(
    (kind: TemplateKind, opts?: { toGraph?: boolean }) => {
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
    },
    [allowInlineKeys, bearerEnv, defaults, setComposerJson, setComposerMode, setTemplateKind, setGraphState, targets],
  );

  const submitDemoAndWait = React.useCallback(async () => {
    const template = buildAgentParallelDemoTemplate(defaults, targets, bearerEnv, allowInlineKeys);
    setTemplateKind("agent_parallel_demo");
    const cleaned = JSON.stringify(template, null, 2);
    setComposerJson(cleaned);
    setSubmitError(null);
    setSubmitResult(null);
    await submitWorkflow(template, { wait: true });
  }, [allowInlineKeys, bearerEnv, defaults, setComposerJson, setTemplateKind, submitWorkflow, targets]);

  const formatJson = React.useCallback(() => {
    try {
      const parsed = JSON.parse(composerJson || "{}");
      setComposerJson(JSON.stringify(parsed, null, 2));
      setSubmitError(null);
    } catch (err) {
      setSubmitError(`Invalid JSON: ${String(err)}`);
    }
  }, [composerJson, setComposerJson]);

  const importGraphFromJson = React.useCallback(() => {
    try {
      const parsed = parseWorkflowToGraph(composerJson || "{}");
      setGraphState(parsed.state);
      setGraphParseWarnings(parsed.warnings);
      setSubmitError(null);
    } catch (err) {
      setSubmitError(String(err));
    }
  }, [composerJson, setGraphState]);

  const exportGraphToJson = React.useCallback(() => {
    if (!graphBuild.result) {
      setSubmitError(graphBuild.error || "Graph is invalid.");
      return;
    }
    setComposerJson(JSON.stringify(graphBuild.result.workflow, null, 2));
    setSubmitError(null);
    setComposerMode("json");
  }, [graphBuild.error, graphBuild.result, setComposerJson, setComposerMode]);

  const clearGraphWarnings = React.useCallback(() => {
    setGraphParseWarnings([]);
  }, []);

  const submit = React.useCallback(async () => {
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
  }, [composerJson, composerMode, graphBuild.error, graphBuild.result, setComposerJson, submitWorkflow]);

  return {
    allowInlineKeys,
    applyTemplate,
    bearerEnv,
    cancelBusy,
    cancelWorkflow,
    clearGraphWarnings,
    clearPersistedWait,
    composerJson,
    composerMode,
    defaults,
    exportGraphToJson,
    formatJson,
    graphBuild,
    graphParseWarnings,
    graphState,
    importGraphFromJson,
    resumePersistedWait,
    serverWaitStatus,
    setAllowInlineKeys,
    setComposerJson,
    setComposerMode,
    setGraphState,
    submit,
    submitBusy,
    submitDemoAndWait,
    submitError,
    submitResult,
    submitWorkflow,
    targets,
    templateKind,
    setTemplateKind,
    waitPersisted,
    waitPersistedExtra,
    waitState,
  };
}
