import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import type { Caps, DaemonConfigResp } from "../api";
import {
  apiBrokerListAgents,
  apiBrokerListDeployments,
  apiGetDiagnostics,
  apiGetDiagnosticsProviders,
  apiGetOpenRouterModels,
  apiGetModeratorEvents,
  apiPostModeratorDirective,
  apiPostModeratorTask,
  apiPostDiagnosticsProviderTest,
} from "../api";
import type { ModeratorEvent } from "../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../hooks/useUiSettings";
import useLocalStorageState from "../hooks/useLocalStorageState";
import FieldLabel from "./FieldLabel";

function SectionHeader({ title, action }: { title: string; action?: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <div className="text-xs font-semibold text-white/70">{title}</div>
      {action ? <div>{action}</div> : null}
    </div>
  );
}

function ToggleRow({
  label,
  checked,
  onChange,
  disabled,
}: {
  label: string;
  checked: boolean;
  onChange: (next: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <label className="flex items-center justify-between gap-2">
      <span>{label}</span>
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} disabled={disabled} />
    </label>
  );
}

function formatBytes(n?: number | null) {
  if (!n || !Number.isFinite(n) || n <= 0) return "";
  const units = ["B", "KB", "MB", "GB"];
  let idx = 0;
  let v = n;
  while (v >= 1024 && idx < units.length - 1) {
    v /= 1024;
    idx++;
  }
  const rounded = idx === 0 ? v.toFixed(0) : v.toFixed(2);
  return `${rounded} ${units[idx]}`;
}

function formatDuration(ms?: number | null) {
  if (!ms || !Number.isFinite(ms) || ms <= 0) return "";
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const rem = s % 60;
  if (h > 0) return `${h}h ${m}m ${rem}s`;
  if (m > 0) return `${m}m ${rem}s`;
  return `${rem}s`;
}

function truncateText(value: string, maxLen: number) {
  if (value.length <= maxLen) return value;
  return `${value.slice(0, Math.max(0, maxLen - 1))}…`;
}

function formatDiffValue(value: unknown) {
  if (value === undefined) return "(undefined)";
  if (value === null) return "null";
  if (typeof value === "string") return truncateText(JSON.stringify(value), 200);
  try {
    return truncateText(JSON.stringify(value), 200);
  } catch {
    return truncateText(String(value), 200);
  }
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

type JsonDiffEntry = { path: string; a: unknown; b: unknown };

function collectJsonDiffs(a: unknown, b: unknown, path: string, out: JsonDiffEntry[], maxDiffs: number) {
  if (out.length >= maxDiffs) return;
  if (a === b) return;
  const aIsArr = Array.isArray(a);
  const bIsArr = Array.isArray(b);
  if (aIsArr || bIsArr) {
    if (!aIsArr || !bIsArr) {
      out.push({ path: path || "<root>", a, b });
      return;
    }
    const max = Math.max(a.length, b.length);
    for (let i = 0; i < max; i += 1) {
      collectJsonDiffs(a[i], b[i], `${path}[${i}]`, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    const keys = new Set<string>([...Object.keys(a), ...Object.keys(b)]);
    for (const key of keys) {
      const nextPath = path ? `${path}.${key}` : key;
      collectJsonDiffs(a[key], b[key], nextPath, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  out.push({ path: path || "<root>", a, b });
}

function formatModeratorEventSummary(event: ModeratorEvent) {
  const type = typeof event?.type === "string" ? event.type : "";
  const data = event?.data && typeof event.data === "object" ? (event.data as any) : {};
  if (type === "moderator_directive") {
    const directive = typeof data?.directive === "string" ? data.directive : "";
    return directive || "(directive)";
  }
  if (type === "moderator_task_published") {
    const task = data?.task && typeof data.task === "object" ? data.task : {};
    const title = typeof task?.title === "string" ? task.title : "";
    return title || "(task)";
  }
  return "";
}

async function copyTextToClipboard(text: string): Promise<boolean> {
  if (!text) return false;
  try {
    if (navigator.clipboard && typeof navigator.clipboard.writeText === "function") {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // fallback below
  }
  try {
    const el = document.createElement("textarea");
    el.value = text;
    el.setAttribute("readonly", "true");
    el.style.position = "absolute";
    el.style.left = "-9999px";
    document.body.appendChild(el);
    el.select();
    const ok = document.execCommand("copy");
    document.body.removeChild(el);
    return ok;
  } catch {
    // ignore
  }
  return false;
}

function parseCsvList(raw: string) {
  const out: string[] = [];
  if (!raw) return out;
  const seen = new Set<string>();
  for (const entry of raw.split(",")) {
    const trimmed = entry.trim();
    if (!trimmed || seen.has(trimmed)) continue;
    seen.add(trimmed);
    out.push(trimmed);
  }
  return out;
}

function appendCsvValue(raw: string, value: string) {
  const trimmed = value.trim();
  if (!trimmed) return raw;
  const items = parseCsvList(raw);
  if (items.includes(trimmed)) return items.join(", ");
  items.push(trimmed);
  return items.join(", ");
}

type SettingsDrawerProps = {
  open: boolean;
  onClose: () => void;
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  session: {
    id: string;
    setId: (next: string) => void;
    sessions: string[];
    refresh: () => void;
    newSession: () => void;
    newSessionPending: boolean;
    deleteSession: (sid: string) => void;
    deletePending: boolean;
    deleteError: string | null;
    clearAll: () => void;
    clearAllPending: boolean;
    clearAllError: string | null;
  };
  daemonConfig: {
    data?: DaemonConfigResp;
    isFetching: boolean;
    refresh: () => void;
  };
  updateDaemonDefaults: {
    pending: boolean;
    error: string | null;
    success: boolean;
    saveDefaults: () => void;
    saveApiKey: () => void;
    clearApiKey: () => void;
  };
  caps: {
    data?: Caps;
    source: "live" | "cache" | "none";
    updatedMs?: number;
    isFetching: boolean;
    error: string | null;
    refresh: () => void;
  };
};

export default function SettingsDrawer(props: SettingsDrawerProps) {
  const { connection, run, client } = props;
  const [clearAllArmed, setClearAllArmed] = React.useState<boolean>(false);
  const clearAllArmTimeoutRef = React.useRef<number>(0);
  const [brokerAgentsBusy, setBrokerAgentsBusy] = React.useState<boolean>(false);
  const [brokerAgentsError, setBrokerAgentsError] = React.useState<string | null>(null);
  const [brokerAgents, setBrokerAgents] = React.useState<any[] | null>(null);
  const [brokerDeploymentsBusy, setBrokerDeploymentsBusy] = React.useState<boolean>(false);
  const [brokerDeploymentsError, setBrokerDeploymentsError] = React.useState<string | null>(null);
  const [brokerDeployments, setBrokerDeployments] = React.useState<any[] | null>(null);
  const [brokerDeploymentsDefaultId, setBrokerDeploymentsDefaultId] = React.useState<string | null>(null);
  const [moderatorDirective, setModeratorDirective] = React.useState<string>("");
  const [moderatorDirectiveScope, setModeratorDirectiveScope] = React.useState<string>("");
  const [moderatorDirectiveAssignees, setModeratorDirectiveAssignees] = React.useState<string>("");
  const [moderatorDirectivePick, setModeratorDirectivePick] = React.useState<string>("");
  const [moderatorTaskTitle, setModeratorTaskTitle] = React.useState<string>("");
  const [moderatorTaskDetail, setModeratorTaskDetail] = React.useState<string>("");
  const [moderatorTaskAssignees, setModeratorTaskAssignees] = React.useState<string>("");
  const [moderatorTaskPick, setModeratorTaskPick] = React.useState<string>("");
  const [moderatorAppendToSession, setModeratorAppendToSession] = React.useState<boolean>(false);
  const [moderatorBusy, setModeratorBusy] = React.useState<boolean>(false);
  const [moderatorError, setModeratorError] = React.useState<string | null>(null);
  const [moderatorSuccess, setModeratorSuccess] = React.useState<string | null>(null);
  const [moderatorEventsAuto, setModeratorEventsAuto] = React.useState<boolean>(false);
  const [moderatorEventsMaxBytes, setModeratorEventsMaxBytes] = React.useState<string>("1048576");
  const [moderatorEventsIncludeDirectives, setModeratorEventsIncludeDirectives] = React.useState<boolean>(true);
  const [moderatorEventsIncludeTasks, setModeratorEventsIncludeTasks] = React.useState<boolean>(true);
  const [moderatorEventsFilter, setModeratorEventsFilter] = React.useState<string>("");
  const [moderatorEventsExpanded, setModeratorEventsExpanded] = React.useState<Record<string, boolean>>({});
  const [moderatorPinnedStore, setModeratorPinnedStore] = useLocalStorageState<
    Record<string, Record<string, ModeratorEvent>>
  >("agentui.moderatorPinnedEvents", {});
  const [copyNotice, setCopyNotice] = React.useState<string | null>(null);
  const [pinNotice, setPinNotice] = React.useState<string | null>(null);
  const [pinError, setPinError] = React.useState<string | null>(null);
  const [pinnedCompareA, setPinnedCompareA] = React.useState<string>("");
  const [pinnedCompareB, setPinnedCompareB] = React.useState<string>("");
  const [pinnedCompareDiffOnly, setPinnedCompareDiffOnly] = React.useState<boolean>(false);
  const copyNoticeTimeoutRef = React.useRef<number>(0);
  const pinNoticeTimeoutRef = React.useRef<number>(0);
  const pinImportRef = React.useRef<HTMLInputElement | null>(null);
  const serverPrefsBase = String(connection.serverPrefsBase || "").trim();
  const serverPrefsCanSync = serverPrefsBase.length > 0;
  const serverPrefsTarget = connection.mode === "broker" ? "broker" : "daemon";
  const serverPrefsStatusLabel =
    connection.serverPrefsStatus === "loading" ? "syncing…" : connection.serverPrefsStatus;
  const serverPrefsAutoNote = connection.serverPrefsAuto
    ? (() => {
        switch (connection.serverPrefsAutoStatus) {
          case "checking":
            return "Auto: checking server support…";
          case "ready":
            return "Auto: server-side sync enabled.";
          case "auth_required":
            return "Auto: add an auth token to enable server-side sync.";
          case "unsupported":
            return "Auto: server does not advertise client prefs support.";
          case "error":
            return `Auto: check failed (${connection.serverPrefsAutoError || "unknown error"}).`;
          default:
            return "Auto: waiting for server info…";
        }
      })()
    : null;
  const [openrouterModels, setOpenrouterModels] = React.useState<any | null>(null);

  React.useEffect(() => {
    if (!props.open) setClearAllArmed(false);
  }, [props.open]);

  React.useEffect(() => {
    return () => {
      if (clearAllArmTimeoutRef.current) {
        try {
          window.clearTimeout(clearAllArmTimeoutRef.current);
        } catch {
          // ignore
        }
      }
      clearAllArmTimeoutRef.current = 0;
      if (copyNoticeTimeoutRef.current) {
        try {
          window.clearTimeout(copyNoticeTimeoutRef.current);
        } catch {
          // ignore
        }
        copyNoticeTimeoutRef.current = 0;
      }
      if (pinNoticeTimeoutRef.current) {
        try {
          window.clearTimeout(pinNoticeTimeoutRef.current);
        } catch {
          // ignore
        }
        pinNoticeTimeoutRef.current = 0;
      }
    };
  }, []);

  React.useEffect(() => {
    setBrokerAgents(null);
    setBrokerAgentsError(null);
  }, [connection.brokerBase, connection.brokerAuthToken]);

  React.useEffect(() => {
    setBrokerDeployments(null);
    setBrokerDeploymentsError(null);
    setBrokerDeploymentsDefaultId(null);
  }, [connection.brokerBase, connection.brokerAuthToken, connection.brokerAgentId]);

  const listBrokerAgents = React.useCallback(async () => {
    setBrokerAgentsError(null);
    setBrokerAgentsBusy(true);
    try {
      const bb = String(connection.brokerBase || "").trim().replace(/\/+$/, "");
      const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
      const r = await apiBrokerListAgents(withScheme, { mode: "broker", token: connection.brokerAuthToken });
      const agents = Array.isArray((r as any)?.agents) ? ((r as any).agents as any[]) : [];
      setBrokerAgents(agents);
      if (!String(connection.brokerAgentId || "").trim()) {
        const connected = agents.find((a) => a && a.connected === true);
        if (connected && typeof connected.agent_id === "string") connection.setBrokerAgentId(connected.agent_id);
      }
    } catch (e) {
      setBrokerAgentsError(String(e));
      setBrokerAgents(null);
    } finally {
      setBrokerAgentsBusy(false);
    }
  }, [connection]);

  const listBrokerDeployments = React.useCallback(async () => {
    setBrokerDeploymentsError(null);
    setBrokerDeploymentsBusy(true);
    try {
      const bb = String(connection.brokerBase || "").trim().replace(/\/+$/, "");
      const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
      const agentId = String(connection.brokerAgentId || "").trim();
      if (!agentId) {
        throw new Error("missing agent_id");
      }
      const r = await apiBrokerListDeployments(withScheme, agentId, { mode: "broker", token: connection.brokerAuthToken });
      const deployments = Array.isArray((r as any)?.deployments) ? ((r as any).deployments as any[]) : [];
      setBrokerDeployments(deployments);
      const defaultId = typeof (r as any)?.default_deployment_id === "string" ? String((r as any).default_deployment_id) : "";
      setBrokerDeploymentsDefaultId(defaultId || null);
    } catch (e) {
      setBrokerDeploymentsError(String(e));
      setBrokerDeployments(null);
      setBrokerDeploymentsDefaultId(null);
    } finally {
      setBrokerDeploymentsBusy(false);
    }
  }, [connection]);

  const fetchOpenRouterModels = useMutation({
    mutationFn: async () => {
      const minTotal = Number(run.orMinTotal);
      const maxTotal = Number(run.orMaxTotal);
      const limit = Number(run.orLimit);
      return apiGetOpenRouterModels(connection.effectiveBase, {
        daemonAuth: connection.daemonAuth,
        apiKey: run.apiKey || undefined,
        openrouterBaseUrl: "https://openrouter.ai/api/v1",
        minTotal: Number.isFinite(minTotal) ? minTotal : 0.01,
        maxTotal: Number.isFinite(maxTotal) ? maxTotal : 0.5,
        requireMultimodalInput: run.orRequireMultimodal,
        requireTools: run.orRequireTools,
        includeFree: false,
        limit: Number.isFinite(limit) ? limit : 50,
        refresh: true,
      });
    },
    onSuccess: (v) => {
      setOpenrouterModels(v);
      if (v.ok && v.recommended_model && typeof v.recommended_model === "string" && v.recommended_model.length > 0) {
        run.setModel(v.recommended_model);
        run.setBaseUrl("https://openrouter.ai/api/v1");
      }
    },
  });

  const [providerTests, setProviderTests] = React.useState<Record<string, any>>({});
  const runProviderTest = React.useCallback(
    async (provider: string) => {
      const key = String(provider || "").trim();
      if (!key) return;
      setProviderTests((prev) => ({ ...prev, [key]: { status: "running" } }));
      try {
        const res = await apiPostDiagnosticsProviderTest(
          connection.effectiveBase,
          {
            provider: key,
            prompt: "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
            expect: "40",
            tools: "basic",
            require_tool_call: true,
            timeout_ms: 30000,
            max_steps: 6,
          },
          connection.daemonAuth,
        );
        setProviderTests((prev) => ({
          ...prev,
          [key]: {
            status: res.ok ? "ok" : "error",
            data: res,
            error: res.ok ? null : res.error || "provider test failed",
          },
        }));
      } catch (e) {
        setProviderTests((prev) => ({ ...prev, [key]: { status: "error", error: String(e) } }));
      }
    },
    [connection],
  );

  const providerStatus = (name: string) => {
    const entry = providerTests[name];
    if (!entry) return null;
    if (entry.status === "running") return <span className="text-amber-200">running…</span>;
    if (entry.status === "ok") return <span className="text-emerald-200">ok</span>;
    if (entry.status === "error") return <span className="text-rose-200">error</span>;
    return null;
  };

  const diagnostics = useQuery({
    queryKey: ["diagnostics", connection.effectiveBase, connection.authKey],
    queryFn: () => apiGetDiagnostics(connection.effectiveBase, connection.daemonAuth),
    enabled: props.open,
    retry: 1,
  });
  const diagnosticsProviders = useQuery({
    queryKey: ["diagnosticsProviders", connection.effectiveBase, connection.authKey],
    queryFn: () => apiGetDiagnosticsProviders(connection.effectiveBase, connection.daemonAuth),
    enabled: props.open,
    retry: 1,
  });
  const moderatorEventsTypes = React.useMemo(() => {
    const types: string[] = [];
    if (moderatorEventsIncludeDirectives) types.push("moderator_directive");
    if (moderatorEventsIncludeTasks) types.push("moderator_task_published");
    if (types.length === 0) {
      return ["moderator_directive", "moderator_task_published"];
    }
    return types;
  }, [moderatorEventsIncludeDirectives, moderatorEventsIncludeTasks]);
  const moderatorEventsMaxBytesValue = React.useMemo(() => {
    const parsed = Number.parseInt(String(moderatorEventsMaxBytes || "").trim(), 10);
    if (!Number.isFinite(parsed) || parsed <= 0) return 1024 * 1024;
    return Math.min(parsed, 4 * 1024 * 1024);
  }, [moderatorEventsMaxBytes]);
  const moderatorEventsEnabled = (props.caps.data as any)?.features?.moderator?.events !== false;
  const moderatorEvents = useQuery({
    queryKey: [
      "moderatorEvents",
      connection.effectiveBase,
      connection.authKey,
      props.session.id,
      moderatorEventsMaxBytesValue,
      moderatorEventsTypes.join(","),
    ],
    queryFn: () =>
      apiGetModeratorEvents(connection.effectiveBase, props.session.id, connection.daemonAuth, {
        maxBytes: moderatorEventsMaxBytesValue,
        types: moderatorEventsTypes,
      }),
    enabled: props.open && moderatorEventsAuto && moderatorEventsEnabled && !!props.session.id.trim(),
    retry: 1,
    refetchInterval: moderatorEventsAuto ? 5000 : false,
  });

  const cfg = props.daemonConfig.data;
  const daemonDefaults = cfg?.daemon;
  const baseUrlLabel = String(run.baseUrl || "").trim();
  const diag = diagnostics.data;
  const capsData = props.caps.data;
  const moderatorPinnedScope = React.useMemo(() => {
    const base = String(connection.effectiveBase || "").trim();
    const sid = String(props.session.id || "").trim();
    if (!base || !sid) return "";
    return `${base}::${sid}`;
  }, [connection.effectiveBase, props.session.id]);
  const moderatorPinnedEvents = React.useMemo(() => {
    if (!moderatorPinnedScope) return {};
    return (moderatorPinnedStore[moderatorPinnedScope] ?? {}) as Record<string, ModeratorEvent>;
  }, [moderatorPinnedScope, moderatorPinnedStore]);
  const updateModeratorPinnedEvents = React.useCallback(
    (updater: Record<string, ModeratorEvent> | ((prev: Record<string, ModeratorEvent>) => Record<string, ModeratorEvent>)) => {
      if (!moderatorPinnedScope) return;
      setModeratorPinnedStore((prev) => {
        const current = prev[moderatorPinnedScope] ?? {};
        const nextScope = typeof updater === "function" ? updater(current) : updater;
        const nextStore = { ...prev };
        if (Object.keys(nextScope).length === 0) {
          delete nextStore[moderatorPinnedScope];
        } else {
          nextStore[moderatorPinnedScope] = nextScope;
        }
        return nextStore;
      });
    },
    [moderatorPinnedScope, setModeratorPinnedStore],
  );
  const showPinNotice = React.useCallback((msg: string, ok: boolean) => {
    setPinError(ok ? null : msg);
    setPinNotice(ok ? msg : null);
    if (pinNoticeTimeoutRef.current) {
      try {
        window.clearTimeout(pinNoticeTimeoutRef.current);
      } catch {
        // ignore
      }
    }
    pinNoticeTimeoutRef.current = window.setTimeout(() => {
      setPinNotice(null);
      setPinError(null);
    }, 2000);
  }, []);
  const brokerAgentOptions = React.useMemo(() => {
    const list = Array.isArray(brokerAgents) ? brokerAgents : [];
    return list
      .map((agent) => {
        const id = typeof agent?.agent_id === "string" ? agent.agent_id : "";
        if (!id) return null;
        const connected = agent?.connected === true;
        const label = connected ? `${id} · connected` : id;
        return { id, label, connected };
      })
      .filter(Boolean) as Array<{ id: string; label: string; connected: boolean }>;
  }, [brokerAgents]);
  const capsJson = React.useMemo(() => (capsData ? JSON.stringify(capsData, null, 2) : ""), [capsData]);
  const capsAge = React.useMemo(() => {
    if (!props.caps.updatedMs) return "";
    const age = Math.max(0, Date.now() - props.caps.updatedMs);
    return formatDuration(age);
  }, [props.caps.updatedMs]);
  const jobsEnabled = (capsData as any)?.features?.jobs?.enabled !== false;
  const automationCaps = (capsData as any)?.features?.automation;
  const automationProfiles = React.useMemo(() => {
    const raw = automationCaps?.profiles;
    const list = Array.isArray(raw) ? raw.filter((p: any) => typeof p === "string") : [];
    return list.length ? list : ["full", "guided", "strict", "custom"];
  }, [automationCaps?.profiles]);
  const automationDefault =
    automationCaps && typeof automationCaps.default_profile === "string" ? automationCaps.default_profile : "";
  const automationOverrideAllowed = automationCaps ? automationCaps.per_run_override !== false : true;
  const moderatorCaps = (capsData as any)?.features?.moderator;
  const moderatorDirectivesEnabled = moderatorCaps ? moderatorCaps.directives !== false : true;
  const moderatorTasksEnabled = moderatorCaps ? moderatorCaps.tasks !== false : true;

  const publishModeratorDirective = React.useCallback(async () => {
    const sid = String(props.session.id || "").trim();
    const directive = String(moderatorDirective || "").trim();
    const scope = String(moderatorDirectiveScope || "").trim();
    const assignees = parseCsvList(moderatorDirectiveAssignees);
    if (!sid) {
      setModeratorError("missing session_id");
      return;
    }
    if (!directive) {
      setModeratorError("missing directive");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const req: any = {
        session_id: sid,
        directive,
        append_to_session: moderatorAppendToSession,
        actor: { role: "moderator", id: client.clientId || "moderator", kind: "webui" },
      };
      if (scope) req.scope = scope;
      if (assignees.length > 0) req.assignees = assignees;
      const resp = await apiPostModeratorDirective(
        connection.effectiveBase,
        req,
        connection.daemonAuth,
      );
      if (!resp.ok) throw new Error(resp.error || "failed to publish directive");
      setModeratorDirective("");
      setModeratorSuccess("directive published");
    } catch (err: any) {
      setModeratorError(String(err?.message || err));
    } finally {
      setModeratorBusy(false);
    }
  }, [
    client.clientId,
    connection.daemonAuth,
    connection.effectiveBase,
    moderatorAppendToSession,
    moderatorDirective,
    moderatorDirectiveAssignees,
    moderatorDirectiveScope,
    props.session.id,
  ]);

  const publishModeratorTask = React.useCallback(async () => {
    const sid = String(props.session.id || "").trim();
    const title = String(moderatorTaskTitle || "").trim();
    const detail = String(moderatorTaskDetail || "").trim();
    const assignees = parseCsvList(moderatorTaskAssignees);
    if (!sid) {
      setModeratorError("missing session_id");
      return;
    }
    if (!title) {
      setModeratorError("missing task title");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const req: any = {
        session_id: sid,
        title,
        detail: detail || undefined,
        append_to_session: moderatorAppendToSession,
        actor: { role: "moderator", id: client.clientId || "moderator", kind: "webui" },
      };
      if (assignees.length > 0) req.assignees = assignees;
      const resp = await apiPostModeratorTask(
        connection.effectiveBase,
        req,
        connection.daemonAuth,
      );
      if (!resp.ok) throw new Error(resp.error || "failed to publish task");
      setModeratorTaskTitle("");
      setModeratorTaskDetail("");
      setModeratorSuccess("task published");
    } catch (err: any) {
      setModeratorError(String(err?.message || err));
    } finally {
      setModeratorBusy(false);
    }
  }, [
    client.clientId,
    connection.daemonAuth,
    connection.effectiveBase,
    moderatorAppendToSession,
    moderatorTaskAssignees,
    moderatorTaskDetail,
    moderatorTaskTitle,
    props.session.id,
  ]);

  const addDirectiveAssignee = React.useCallback((value: string) => {
    if (!value) return;
    setModeratorDirectiveAssignees((prev) => appendCsvValue(prev, value));
  }, []);

  const addTaskAssignee = React.useCallback((value: string) => {
    if (!value) return;
    setModeratorTaskAssignees((prev) => appendCsvValue(prev, value));
  }, []);

  const buildRuntimeMemberTaskTemplate = React.useCallback(() => {
    return (
      "Use tool broker_team_runtime_members_update with payload:\\n" +
      "{\\n" +
      "  \\\"team_id\\\": \\\"team-alpha\\\",\\n" +
      "  \\\"team_run_id\\\": \\\"run-123\\\",\\n" +
      "  \\\"mode\\\": \\\"merge\\\",\\n" +
      "  \\\"runtime_members\\\": [\\n" +
      "    {\\\"member_id\\\": \\\"rt-1\\\", \\\"agent_id\\\": \\\"agent-a\\\", \\\"role\\\": \\\"executor\\\"}\\n" +
      "  ]\\n" +
      "}\\n" +
      "Optional: pass broker_base/auth_token if BROKER_BASE_URL / BROKER_AUTH_TOKEN are not set."
    );
  }, []);

  const applyRuntimeMemberTaskTemplate = React.useCallback(() => {
    setModeratorTaskTitle((prev) => (prev.trim().length === 0 ? "Update team run runtime members" : prev));
    const template = buildRuntimeMemberTaskTemplate();
    setModeratorTaskDetail((prev) => {
      const trimmed = prev.trim();
      if (trimmed.length === 0) return template;
      if (trimmed.includes("broker_team_runtime_members_update")) return prev;
      return `${prev}\\n\\n${template}`;
    });
  }, [buildRuntimeMemberTaskTemplate]);
  const diagProviders = diagnosticsProviders.data;
  const providerEntries = diagProviders && diagProviders.providers && typeof diagProviders.providers === "object"
    ? (diagProviders.providers as Record<string, any>)
    : {};
  const deepseekKeyPresent = providerEntries?.deepseek?.key_present === true;
  const moonshotKeyPresent = providerEntries?.moonshot?.key_present === true;
  const moderatorEventsData = moderatorEvents.data;
  const moderatorEventsError = moderatorEvents.isError ? String(moderatorEvents.error || "failed to load events") : null;
  const moderatorEventsList = Array.isArray(moderatorEventsData?.events) ? (moderatorEventsData?.events as ModeratorEvent[]) : [];
  const moderatorEventsFilterValue = String(moderatorEventsFilter || "").trim().toLowerCase();
  const showCopyNotice = React.useCallback(
    (label: string, ok: boolean) => {
      const msg = ok ? `Copied ${label}` : "Copy failed";
      setCopyNotice(msg);
      if (copyNoticeTimeoutRef.current) {
        try {
          window.clearTimeout(copyNoticeTimeoutRef.current);
        } catch {
          // ignore
        }
      }
      copyNoticeTimeoutRef.current = window.setTimeout(() => setCopyNotice(null), 1500);
    },
    [],
  );
  const handleCopy = React.useCallback(
    async (label: string, text: string) => {
      const ok = await copyTextToClipboard(text);
      showCopyNotice(label, ok);
    },
    [showCopyNotice],
  );
  const moderatorEventsFiltered = React.useMemo(() => {
    if (!moderatorEventsFilterValue) return moderatorEventsList;
    return moderatorEventsList.filter((event) => {
      const type = typeof event?.type === "string" ? event.type : "";
      const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
      const actorId = typeof actor?.id === "string" ? actor.id : "";
      const summary = formatModeratorEventSummary(event);
      const haystacks = [type, actorId, summary];
      for (const h of haystacks) {
        if (h && h.toLowerCase().includes(moderatorEventsFilterValue)) return true;
      }
      try {
        const raw = JSON.stringify(event);
        if (raw && raw.toLowerCase().includes(moderatorEventsFilterValue)) return true;
      } catch {
        // ignore
      }
      return false;
    });
  }, [moderatorEventsFilterValue, moderatorEventsList]);
  const moderatorPinnedEntries = React.useMemo(() => {
    const entries = Object.entries(moderatorPinnedEvents);
    entries.sort((a, b) => {
      const ta = typeof a[1]?.ts_unix_ms === "number" ? a[1].ts_unix_ms : 0;
      const tb = typeof b[1]?.ts_unix_ms === "number" ? b[1].ts_unix_ms : 0;
      return tb - ta;
    });
    return entries;
  }, [moderatorPinnedEvents]);
  const pinnedCompareOptions = React.useMemo(() => {
    return moderatorPinnedEntries.map(([key, event]) => {
      const type = typeof event?.type === "string" ? event.type : "event";
      const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
      const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
      const actorId = typeof actor?.id === "string" ? actor.id : "";
      const summary = formatModeratorEventSummary(event);
      const labelParts = [type, actorId, summary].filter(Boolean);
      const label = labelParts.length > 0 ? `${labelParts.join(" · ")}${ts ? ` · ${ts}` : ""}` : ts || key;
      return { key, label };
    });
  }, [moderatorPinnedEntries]);
  React.useEffect(() => {
    if (!pinnedCompareA || !moderatorPinnedEvents[pinnedCompareA]) {
      setPinnedCompareA(pinnedCompareOptions[0]?.key ?? "");
    }
    if (!pinnedCompareB || !moderatorPinnedEvents[pinnedCompareB]) {
      const fallback = pinnedCompareOptions.length > 1 ? pinnedCompareOptions[1]?.key : pinnedCompareOptions[0]?.key ?? "";
      setPinnedCompareB(fallback);
    }
  }, [moderatorPinnedEvents, pinnedCompareA, pinnedCompareB, pinnedCompareOptions]);
  const moderatorRolePresets = React.useMemo(() => ["role:planner", "role:executor", "role:critic"], []);

  if (!props.open) return null;

  return (
    <div className="fixed inset-0 z-40" data-testid="settings-drawer">
      <div className="absolute inset-0 bg-black/60" onClick={props.onClose} role="button" tabIndex={0} />
      <div className="absolute right-0 top-0 h-full w-[520px] max-w-[94vw] overflow-auto border-l border-white/10 bg-slate-950 p-4">
        <div className="flex items-center justify-between gap-3">
          <div className="text-sm font-semibold">Settings</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={props.onClose}
            type="button"
            data-testid="settings-close"
          >
            Close
          </button>
        </div>

        <div className="mt-4">
          <FieldLabel>Connection</FieldLabel>
          <div className="mt-2">
            <FieldLabel>Connection profile</FieldLabel>
            <div className="mt-1 flex flex-wrap items-center gap-2">
              <select
                className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={connection.activeProfileId}
                onChange={(e) => connection.setActiveProfileId(e.target.value)}
              >
                {(connection.profiles || []).map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => connection.addProfile(connection.mode)}
              >
                New
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => connection.duplicateProfile()}
                disabled={(connection.profiles || []).length === 0}
              >
                Duplicate
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={(connection.profiles || []).length <= 1}
                onClick={() => {
                  if ((connection.profiles || []).length <= 1) return;
                  const name = connection.profileName || "profile";
                  if (!window.confirm(`Delete connection profile \"${name}\"?`)) return;
                  connection.deleteProfile(connection.activeProfileId);
                }}
              >
                Delete
              </button>
            </div>
            <input
              className="mt-2 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={connection.profileName}
              onChange={(e) => connection.setProfileName(e.target.value)}
              placeholder="Profile name"
            />
            <div className="mt-2 text-[11px] text-white/60">
              Profiles are cached locally and can sync to the server for cross-device persistence.
            </div>
            <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
              <div className="text-xs font-semibold text-white/70">Server profile sync</div>
              <div className="mt-1 flex items-center justify-between gap-2">
                <label className="flex items-center gap-2">
                  <input
                    type="checkbox"
                    checked={connection.serverPrefsEnabled}
                    onChange={(e) => connection.setServerPrefsEnabled(e.target.checked)}
                    disabled={!serverPrefsCanSync}
                  />
                  <span>
                    Sync connection profiles to {serverPrefsTarget} (no tokens)
                    {connection.serverPrefsAuto && !connection.serverPrefsUserSet ? " · auto" : ""}
                  </span>
                </label>
              </div>
              <div className="mt-1 text-white/50">
                {serverPrefsCanSync
                  ? `Status: ${serverPrefsStatusLabel}${connection.serverPrefsLastSyncMs ? ` · ${new Date(connection.serverPrefsLastSyncMs).toLocaleString()}` : ""}`
                  : "Set a base URL first."}
              </div>
              {serverPrefsAutoNote ? <div className="mt-1 text-white/50">{serverPrefsAutoNote}</div> : null}
              {connection.serverPrefsError ? (
                <div className="mt-1 text-rose-200">Sync error: {connection.serverPrefsError}</div>
              ) : null}
              <div className="mt-3">
                <FieldLabel>Server prefs client id</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={client.clientId}
                  onChange={(e) => client.setClientId(e.target.value)}
                  placeholder="e.g. webui"
                />
                <div className="mt-1 text-white/50">
                  Use a stable id to share profiles across devices (server-side). Leave as default for per-device storage.
                </div>
                <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/70">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => client.setClientId("webui")}
                  >
                    Use shared id
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() =>
                      client.setClientId(`webui-${Date.now()}-${Math.random().toString(16).slice(2)}`)
                    }
                  >
                    New random id
                  </button>
                </div>
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2">
                {connection.serverPrefsAuto && connection.serverPrefsUserSet ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() => connection.clearServerPrefsOverride()}
                    disabled={!serverPrefsCanSync}
                    title="Resume auto sync behavior"
                  >
                    Use auto
                  </button>
                ) : null}
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => connection.pullServerPrefs()}
                  disabled={!serverPrefsCanSync || !connection.serverPrefsEnabled}
                  title={serverPrefsBase ? `Pull from ${serverPrefsBase}` : "Set a daemon base URL first"}
                >
                  Pull
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => connection.pushServerPrefs()}
                  disabled={!serverPrefsCanSync || !connection.serverPrefsEnabled}
                  title={serverPrefsBase ? `Push to ${serverPrefsBase}` : "Set a daemon base URL first"}
                >
                  Push
                </button>
              </div>
            </div>
          </div>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={connection.mode}
            onChange={(e) => connection.setMode(e.target.value as any)}
          >
            <option value="direct">direct (agentd)</option>
            <option value="broker">broker (OIDC + agent_id)</option>
          </select>
          <div className="mt-2 text-[11px] text-white/60">
            {connection.mode === "direct"
              ? "Direct: the browser calls agentd over HTTP."
              : "Broker: the browser calls a broker (OIDC), which proxies to a connected agent by id."}
          </div>
        </div>

        {connection.mode === "direct" ? (
          <>
            <div className="mt-4">
              <FieldLabel>Daemon base URL</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                data-testid="daemon-base"
                value={connection.base}
                onChange={(e) => connection.setBase(e.target.value)}
              />
            </div>

            <div className="mt-4">
              <FieldLabel>Daemon auth token (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                data-testid="daemon-auth-token"
                placeholder='Bearer token (e.g. "dev-agentd-token" in docker-compose)'
                value={connection.daemonAuthToken}
                onChange={(e) => connection.setDaemonAuthToken(e.target.value)}
              />
            </div>
          </>
        ) : (
          <>
            <div className="mt-4">
              <FieldLabel>Broker base URL</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={connection.brokerBase}
                onChange={(e) => connection.setBrokerBase(e.target.value)}
                placeholder='e.g. "https://broker.example.com" (or "https://127.0.0.1:8443" in docker-compose)'
              />
            </div>

            <div className="mt-4">
              <FieldLabel>Broker auth token (OIDC)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="Authorization bearer token for broker (OIDC JWT)"
                value={connection.brokerAuthToken}
                onChange={(e) => connection.setBrokerAuthToken(e.target.value)}
              />
              <div className="mt-2 text-[11px] text-white/60">
                Uses <code className="font-mono">Authorization: Bearer &lt;jwt&gt;</code> to call broker endpoints.
              </div>
            </div>

            <div className="mt-4">
              <FieldLabel>Agent id</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={connection.brokerAgentId}
                onChange={(e) => connection.setBrokerAgentId(e.target.value)}
                placeholder='e.g. "agent1"'
              />
              <div className="mt-2 flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  disabled={brokerAgentsBusy || String(connection.brokerAuthToken || "").trim().length === 0}
                  onClick={() => void listBrokerAgents()}
                  title="Fetches /v1/agents from the broker (OIDC required)."
                >
                  {brokerAgentsBusy ? "Listing…" : "List agents"}
                </button>
                <div className="text-[11px] text-white/60">
                  Proxy base: <code className="font-mono text-white/70">{String(connection.effectiveBase || "").trim()}</code>
                </div>
              </div>
              {brokerAgentsError ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                  List agents failed: {brokerAgentsError}
                </div>
              ) : null}
              {brokerAgents && brokerAgents.length > 0 ? (
                <div className="mt-2 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/20">
                  {brokerAgents.map((a: any) => {
                    const id = typeof a?.agent_id === "string" ? a.agent_id : "";
                    if (!id) return null;
                    const connected = a?.connected === true;
                    const lastSeen = typeof a?.last_seen_unix_ms === "number" ? a.last_seen_unix_ms : 0;
                    const selected = String(connection.brokerAgentId || "").trim() === id;
                    return (
                      <button
                        key={id}
                        type="button"
                        className={[
                          "flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5",
                          selected ? "bg-white/10" : "",
                        ].join(" ")}
                        onClick={() => {
                          connection.setBrokerAgentId(id);
                          if (id !== String(connection.brokerAgentId || "").trim()) {
                            connection.setBrokerDeploymentId("");
                          }
                        }}
                        title={a?.remote_addr ? `remote=${String(a.remote_addr)}` : ""}
                      >
                        <span className="font-mono text-white/80">{id}</span>
                        <span className="text-white/60">
                          {connected ? <span className="text-emerald-300">connected</span> : <span className="text-white/40">disconnected</span>}
                          {lastSeen ? ` · last_seen=${new Date(lastSeen).toLocaleString()}` : ""}
                        </span>
                      </button>
                    );
                  })}
                </div>
              ) : null}
            </div>

            <div className="mt-4">
              <FieldLabel>Deployment id (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={connection.brokerDeploymentId}
                onChange={(e) => connection.setBrokerDeploymentId(e.target.value)}
                placeholder='e.g. "laptop-1" (leave blank to auto-pick latest)'
              />
              <div className="mt-2 flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  disabled={
                    brokerDeploymentsBusy ||
                    String(connection.brokerAuthToken || "").trim().length === 0 ||
                    String(connection.brokerAgentId || "").trim().length === 0
                  }
                  onClick={() => void listBrokerDeployments()}
                  title="Fetches /v1/agents/{agent_id}/deployments from the broker."
                >
                  {brokerDeploymentsBusy ? "Listing…" : "List deployments"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => connection.setBrokerDeploymentId("")}
                  title="Clear deployment id to use broker default"
                >
                  Use default
                </button>
                <div className="text-[11px] text-white/60">
                  Uses <code className="font-mono">X-Agentd-Deployment</code>; empty means “latest connected”.
                </div>
              </div>
              {brokerDeploymentsDefaultId ? (
                <div className="mt-2 text-[11px] text-white/60">
                  Broker default: <span className="font-mono text-white/80">{brokerDeploymentsDefaultId}</span>
                </div>
              ) : null}
              {brokerDeploymentsError ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                  List deployments failed: {brokerDeploymentsError}
                </div>
              ) : null}
              {brokerDeployments && brokerDeployments.length > 0 ? (
                <div className="mt-2 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/20">
                  {brokerDeployments.map((d: any) => {
                    const id = typeof d?.deployment_id === "string" ? d.deployment_id : "";
                    if (!id) return null;
                    const connected = d?.connected === true;
                    const lastSeen = typeof d?.last_seen_unix_ms === "number" ? d.last_seen_unix_ms : 0;
                    const selected = String(connection.brokerDeploymentId || "").trim() === id;
                    const isDefault = brokerDeploymentsDefaultId && brokerDeploymentsDefaultId === id;
                    return (
                      <button
                        key={id}
                        type="button"
                        className={[
                          "flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5",
                          selected ? "bg-white/10" : "",
                        ].join(" ")}
                        onClick={() => connection.setBrokerDeploymentId(id)}
                        title={d?.remote_addr ? `remote=${String(d.remote_addr)}` : ""}
                      >
                        <span className="font-mono text-white/80">{id}</span>
                        <span className="text-white/60">
                          {isDefault ? <span className="text-sky-300">default</span> : null}
                          {isDefault ? " · " : ""}
                          {connected ? <span className="text-emerald-300">connected</span> : <span className="text-white/40">disconnected</span>}
                          {lastSeen ? ` · last_seen=${new Date(lastSeen).toLocaleString()}` : ""}
                        </span>
                      </button>
                    );
                  })}
                </div>
              ) : null}
            </div>

            <div className="mt-4">
              <FieldLabel>Agentd auth token (pass-through)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder='Bearer token forwarded to agentd as "X-Agentd-Authorization" (e.g. "dev-agentd-token")'
                value={connection.daemonAuthToken}
                onChange={(e) => connection.setDaemonAuthToken(e.target.value)}
              />
              <div className="mt-2 text-[11px] text-white/60">
                Uses <code className="font-mono">X-Agentd-Authorization: Bearer &lt;token&gt;</code> on proxied requests.
              </div>
            </div>
          </>
        )}

        <div className="mt-4 grid grid-cols-2 gap-3">
          <div className="col-span-2">
            <FieldLabel>Session</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={props.session.id}
              onChange={(e) => props.session.setId(e.target.value)}
            />
          </div>
          <div>
            <FieldLabel>Tools</FieldLabel>
            <select
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.tools}
              onChange={(e) => run.setTools(e.target.value as any)}
            >
              <option value="host">host</option>
              <option value="basic">basic</option>
              <option value="none">none</option>
            </select>
          </div>
          <div>
            <FieldLabel>Host policy</FieldLabel>
            <select
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.hostPolicy}
              onChange={(e) => run.setHostPolicy(e.target.value as any)}
              disabled={run.tools !== "host"}
            >
              <option value="full">full</option>
              <option value="readonly">readonly</option>
            </select>
          </div>
          <div className="col-span-2">
            <FieldLabel>Automation profile</FieldLabel>
            <select
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.automationProfile}
              onChange={(e) => run.setAutomationProfile(e.target.value)}
              disabled={!automationOverrideAllowed}
            >
              <option value="">
                {automationDefault ? `default (${automationDefault})` : "default (daemon config)"}
              </option>
              {automationProfiles.map((p) => (
                <option key={p} value={p}>
                  {p}
                </option>
              ))}
            </select>
            <div className="mt-1 text-[11px] text-white/60">
              Overrides yolo/host policy/policy mode when set. Use default to follow daemon config.
            </div>
            {!automationOverrideAllowed ? (
              <div className="mt-1 text-[11px] text-amber-200">Per-run automation override disabled by daemon caps.</div>
            ) : null}
          </div>
        </div>

        <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <SectionHeader title="Moderator" />
          <div className="mt-2 grid gap-3 text-[11px] text-white/70">
            <div>
              <FieldLabel>Directive</FieldLabel>
              <textarea
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="Publish a nonblocking moderator directive"
                rows={3}
                value={moderatorDirective}
                onChange={(e) => setModeratorDirective(e.target.value)}
              />
            </div>
            <div>
              <FieldLabel>Directive scope (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="e.g. all, team:ops, agent:planner"
                value={moderatorDirectiveScope}
                onChange={(e) => setModeratorDirectiveScope(e.target.value)}
              />
              <div className="mt-1 text-[11px] text-white/50">
                Scope is an advisory label used by collaborating agents to route directives.
              </div>
            </div>
            <div>
              <FieldLabel>Directive assignees (comma-separated, optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="agent-a, agent-b, role:planner"
                value={moderatorDirectiveAssignees}
                onChange={(e) => setModeratorDirectiveAssignees(e.target.value)}
              />
              <div className="mt-1 text-[11px] text-white/50">
                Leave empty to broadcast to all listening agents.
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                <span>Quick roles:</span>
                {moderatorRolePresets.map((role) => (
                  <button
                    key={`moderator-directive-role-${role}`}
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => addDirectiveAssignee(role)}
                  >
                    {role}
                  </button>
                ))}
              </div>
              {connection.mode === "broker" ? (
                <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                  <select
                    className="min-w-[200px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                    value={moderatorDirectivePick}
                    onChange={(e) => setModeratorDirectivePick(e.target.value)}
                    disabled={brokerAgentOptions.length === 0}
                  >
                    <option value="">(pick broker agent)</option>
                    {brokerAgentOptions.map((opt) => (
                      <option key={`moderator-directive-agent-${opt.id}`} value={opt.id}>
                        {opt.label}
                      </option>
                    ))}
                  </select>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() => addDirectiveAssignee(moderatorDirectivePick)}
                    disabled={!moderatorDirectivePick}
                  >
                    Add
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() => void listBrokerAgents()}
                    disabled={brokerAgentsBusy}
                  >
                    {brokerAgentsBusy ? "Refreshing…" : "Refresh agents"}
                  </button>
                </div>
              ) : null}
            </div>
            <div>
              <FieldLabel>Task</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="Task title"
                value={moderatorTaskTitle}
                onChange={(e) => setModeratorTaskTitle(e.target.value)}
              />
              <textarea
                className="mt-2 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                placeholder="Task detail (optional)"
                rows={2}
                value={moderatorTaskDetail}
                onChange={(e) => setModeratorTaskDetail(e.target.value)}
              />
              <div className="mt-2">
                <FieldLabel>Task assignees (comma-separated, optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  placeholder="agent-a, agent-b, role:executor"
                  value={moderatorTaskAssignees}
                  onChange={(e) => setModeratorTaskAssignees(e.target.value)}
                />
                <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                  <span>Quick roles:</span>
                  {moderatorRolePresets.map((role) => (
                    <button
                      key={`moderator-task-role-${role}`}
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                      type="button"
                      onClick={() => addTaskAssignee(role)}
                    >
                      {role}
                    </button>
                  ))}
                </div>
                {connection.mode === "broker" ? (
                  <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                    <select
                      className="min-w-[200px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={moderatorTaskPick}
                      onChange={(e) => setModeratorTaskPick(e.target.value)}
                      disabled={brokerAgentOptions.length === 0}
                    >
                      <option value="">(pick broker agent)</option>
                      {brokerAgentOptions.map((opt) => (
                        <option key={`moderator-task-agent-${opt.id}`} value={opt.id}>
                          {opt.label}
                        </option>
                      ))}
                    </select>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => addTaskAssignee(moderatorTaskPick)}
                      disabled={!moderatorTaskPick}
                    >
                      Add
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => void listBrokerAgents()}
                      disabled={brokerAgentsBusy}
                    >
                      {brokerAgentsBusy ? "Refreshing…" : "Refresh agents"}
                    </button>
                  </div>
                ) : null}
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                  type="button"
                  onClick={() => applyRuntimeMemberTaskTemplate()}
                  disabled={moderatorBusy || !moderatorTasksEnabled}
                >
                  Insert runtime member update template
                </button>
              </div>
            </div>
            <label className="flex items-center justify-between gap-2">
              <span>Append to session history</span>
              <input
                type="checkbox"
                checked={moderatorAppendToSession}
                onChange={(e) => setModeratorAppendToSession(e.target.checked)}
              />
            </label>
            <div className="flex flex-wrap gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => void publishModeratorDirective()}
                disabled={moderatorBusy || !props.session.id.trim() || !moderatorDirectivesEnabled}
              >
                Publish directive
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => void publishModeratorTask()}
                disabled={moderatorBusy || !props.session.id.trim() || !moderatorTasksEnabled}
              >
                Publish task
              </button>
              {moderatorBusy ? <span className="text-white/50">publishing…</span> : null}
            </div>
            {!moderatorDirectivesEnabled || !moderatorTasksEnabled ? (
              <div className="text-amber-200">Moderator publishing disabled by daemon caps.</div>
            ) : null}
            {moderatorError ? <div className="text-rose-200">moderator error: {moderatorError}</div> : null}
            {moderatorSuccess ? <div className="text-emerald-200">{moderatorSuccess}</div> : null}
            <div className="mt-3 rounded-md border border-white/10 bg-black/30 p-2">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="text-xs font-semibold text-white/70">Moderator events</div>
                <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    onClick={() => void moderatorEvents.refetch()}
                    disabled={!moderatorEventsEnabled || !props.session.id.trim() || moderatorEvents.isFetching}
                  >
                    {moderatorEvents.isFetching ? "Loading…" : "Load"}
                  </button>
                  <label className="flex items-center gap-2">
                    <span>auto</span>
                    <input
                      type="checkbox"
                      checked={moderatorEventsAuto}
                      onChange={(e) => setModeratorEventsAuto(e.target.checked)}
                      disabled={!moderatorEventsEnabled || !props.session.id.trim()}
                    />
                  </label>
                </div>
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                <label className="flex items-center gap-2">
                  <input
                    type="checkbox"
                    checked={moderatorEventsIncludeDirectives}
                    onChange={(e) => setModeratorEventsIncludeDirectives(e.target.checked)}
                  />
                  directives
                </label>
                <label className="flex items-center gap-2">
                  <input
                    type="checkbox"
                    checked={moderatorEventsIncludeTasks}
                    onChange={(e) => setModeratorEventsIncludeTasks(e.target.checked)}
                  />
                  tasks
                </label>
                <label className="flex items-center gap-2">
                  <span>max bytes</span>
                  <input
                    className="w-28 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                    value={moderatorEventsMaxBytes}
                    onChange={(e) => setModeratorEventsMaxBytes(e.target.value)}
                  />
                </label>
                <label className="flex items-center gap-2">
                  <span>filter</span>
                  <input
                    className="w-40 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                    value={moderatorEventsFilter}
                    onChange={(e) => setModeratorEventsFilter(e.target.value)}
                    placeholder="type/actor/text"
                  />
                </label>
                {moderatorEventsFilterValue ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setModeratorEventsFilter("")}
                  >
                    Clear
                  </button>
                ) : null}
              </div>
              {moderatorPinnedEntries.length > 0 ? (
                <div className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
                  <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
                    <span>Pinned events ({moderatorPinnedEntries.length})</span>
                    <div className="flex flex-wrap items-center gap-2">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => {
                          try {
                            const payload = JSON.stringify(moderatorPinnedEvents, null, 2);
                            const blob = new Blob([payload], { type: "application/json" });
                            const url = URL.createObjectURL(blob);
                            const a = document.createElement("a");
                            a.href = url;
                            a.download = `moderator_pins_${Date.now()}.json`;
                            a.click();
                            URL.revokeObjectURL(url);
                            showPinNotice("Exported pins", true);
                          } catch (err: any) {
                            showPinNotice(String(err?.message || "export failed"), false);
                          }
                        }}
                      >
                        Export pins
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => pinImportRef.current?.click()}
                      >
                        Import pins
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => updateModeratorPinnedEvents({})}
                      >
                        Clear pins
                      </button>
                    </div>
                  </div>
                  <input
                    ref={pinImportRef}
                    type="file"
                    accept="application/json"
                    className="hidden"
                    onChange={async (e) => {
                      const file = e.target.files?.[0];
                      if (!file) return;
                      try {
                        const text = await file.text();
                        const parsed = JSON.parse(text);
                        let nextPins: Record<string, ModeratorEvent> = {};
                        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
                          for (const [k, v] of Object.entries(parsed as Record<string, unknown>)) {
                            if (v && typeof v === "object") {
                              nextPins[k] = v as ModeratorEvent;
                            }
                          }
                        } else if (Array.isArray(parsed)) {
                          parsed.forEach((ev, idx) => {
                            if (ev && typeof ev === "object") {
                              nextPins[`import-${idx}`] = ev as ModeratorEvent;
                            }
                          });
                        }
                        updateModeratorPinnedEvents(nextPins);
                        showPinNotice("Imported pins", true);
                      } catch (err: any) {
                        showPinNotice(String(err?.message || "import failed"), false);
                      } finally {
                        if (pinImportRef.current) pinImportRef.current.value = "";
                      }
                    }}
                  />
                  <div className="mt-2 grid gap-2">
                    {moderatorPinnedEntries.map(([key, event]) => {
                      const type = typeof event?.type === "string" ? event.type : "event";
                      const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
                      const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
                      const actorId = typeof actor?.id === "string" ? actor.id : "";
                      const summary = formatModeratorEventSummary(event);
                      return (
                        <div key={`pinned-${key}`} className="rounded-md border border-white/5 bg-black/30 p-2 text-[11px] text-white/70">
                          <div className="flex flex-wrap items-center justify-between gap-2">
                            <div className="text-white/80">
                              {type}
                              {actorId ? ` · ${actorId}` : ""}
                            </div>
                            <div className="text-white/40">{ts}</div>
                          </div>
                          {summary ? <div className="text-white/60">{summary}</div> : null}
                          <div className="mt-1 flex flex-wrap items-center gap-2">
                            {summary ? (
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopy("summary", summary)}
                              >
                                Copy summary
                              </button>
                            ) : null}
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() => void handleCopy("JSON", JSON.stringify(event, null, 2))}
                            >
                              Copy JSON
                            </button>
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() =>
                                updateModeratorPinnedEvents((prev) => {
                                  const next = { ...prev };
                                  delete next[key];
                                  return next;
                                })
                              }
                            >
                              Unpin
                            </button>
                          </div>
                        </div>
                      );
                    })}
                  </div>
                  {moderatorPinnedEntries.length > 1 ? (
                    <div className="mt-3 rounded-md border border-white/10 bg-black/20 p-2">
                      <div className="text-[11px] text-white/70">Compare pinned events</div>
                      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                        <label className="flex items-center gap-2">
                          <span>A</span>
                          <select
                            className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                            value={pinnedCompareA}
                            onChange={(e) => setPinnedCompareA(e.target.value)}
                          >
                            {pinnedCompareOptions.map((opt) => (
                              <option key={`pin-a-${opt.key}`} value={opt.key}>
                                {opt.label}
                              </option>
                            ))}
                          </select>
                        </label>
                        <label className="flex items-center gap-2">
                          <span>B</span>
                          <select
                            className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                            value={pinnedCompareB}
                            onChange={(e) => setPinnedCompareB(e.target.value)}
                          >
                            {pinnedCompareOptions.map((opt) => (
                              <option key={`pin-b-${opt.key}`} value={opt.key}>
                                {opt.label}
                              </option>
                            ))}
                          </select>
                        </label>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                          type="button"
                          onClick={() => {
                            setPinnedCompareA(pinnedCompareB);
                            setPinnedCompareB(pinnedCompareA);
                          }}
                          disabled={!pinnedCompareA || !pinnedCompareB}
                        >
                          Swap
                        </button>
                      </div>
                      {pinnedCompareA && pinnedCompareB ? (
                        (() => {
                            const evA = moderatorPinnedEvents[pinnedCompareA];
                            const evB = moderatorPinnedEvents[pinnedCompareB];
                            const jsonA = JSON.stringify(evA, null, 2);
                            const jsonB = JSON.stringify(evB, null, 2);
                            const same = jsonA === jsonB;
                            const diffText = JSON.stringify({ a: evA, b: evB }, null, 2);
                            const diffs: JsonDiffEntry[] = [];
                            if (!same) {
                              collectJsonDiffs(evA, evB, "", diffs, 200);
                            }
                            return (
                              <div className="mt-2 grid gap-2">
                                <div className={`text-[11px] ${same ? "text-emerald-200" : "text-amber-200"}`}>
                                  {same ? "Pinned events are identical." : "Pinned events differ."}
                                </div>
                                <label className="flex items-center gap-2 text-[11px] text-white/60">
                                  <input
                                    type="checkbox"
                                    checked={pinnedCompareDiffOnly}
                                    onChange={(e) => setPinnedCompareDiffOnly(e.target.checked)}
                                  />
                                  Diff-only view
                                </label>
                                <div className="grid gap-2 md:grid-cols-2">
                                  <div className="rounded-md border border-white/10 bg-black/30 p-2">
                                    <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                                      <span>Event A</span>
                                      <button
                                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                        type="button"
                                        onClick={() => void handleCopy("JSON A", jsonA)}
                                      >
                                        Copy JSON A
                                      </button>
                                    </div>
                                    {!pinnedCompareDiffOnly ? (
                                      <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                                        {jsonA}
                                      </pre>
                                    ) : null}
                                  </div>
                                  <div className="rounded-md border border-white/10 bg-black/30 p-2">
                                    <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                                      <span>Event B</span>
                                      <button
                                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                        type="button"
                                        onClick={() => void handleCopy("JSON B", jsonB)}
                                      >
                                        Copy JSON B
                                      </button>
                                    </div>
                                    {!pinnedCompareDiffOnly ? (
                                      <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                                        {jsonB}
                                      </pre>
                                    ) : null}
                                  </div>
                                </div>
                                {pinnedCompareDiffOnly ? (
                                  <div className="rounded-md border border-white/10 bg-black/30 p-2">
                                    <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                                      <span>Combined diff view</span>
                                      <button
                                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                        type="button"
                                        onClick={() => void handleCopy("diff JSON", diffText)}
                                      >
                                        Copy diff JSON
                                      </button>
                                    </div>
                                    <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                                      {diffText}
                                    </pre>
                                  </div>
                                ) : null}
                                {pinnedCompareDiffOnly ? (
                                  <div className="rounded-md border border-white/10 bg-black/30 p-2">
                                    <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                                      <span>Key diffs</span>
                                      <span>{diffs.length} changes</span>
                                    </div>
                                    {diffs.length === 0 ? (
                                      <div className="mt-2 text-[10px] text-white/40">No key-level differences detected.</div>
                                    ) : (
                                      <div className="mt-2 max-h-64 overflow-auto">
                                        {diffs.slice(0, 200).map((diff, idx) => (
                                          <div key={`diff-${idx}`} className="border-b border-white/5 py-1 last:border-b-0">
                                            <div className="text-[10px] text-white/70">{diff.path}</div>
                                            <div className="mt-1 flex flex-wrap items-center gap-2 text-[10px]">
                                              <span className="rounded-md bg-emerald-500/10 px-2 py-1 text-emerald-200">
                                                A: {formatDiffValue(diff.a)}
                                              </span>
                                              <span className="rounded-md bg-amber-500/10 px-2 py-1 text-amber-200">
                                                B: {formatDiffValue(diff.b)}
                                              </span>
                                            </div>
                                          </div>
                                        ))}
                                      </div>
                                    )}
                                    {diffs.length >= 200 ? (
                                      <div className="mt-2 text-[10px] text-white/40">Diffs truncated at 200 entries.</div>
                                    ) : null}
                                  </div>
                                ) : null}
                              </div>
                            );
                          })()
                        ) : null}
                    </div>
                  ) : null}
                </div>
              ) : null}
              {moderatorEventsError ? <div className="mt-2 text-[11px] text-rose-200">{moderatorEventsError}</div> : null}
              {copyNotice ? <div className="mt-2 text-[11px] text-emerald-200">{copyNotice}</div> : null}
              {pinNotice ? <div className="mt-2 text-[11px] text-emerald-200">{pinNotice}</div> : null}
              {pinError ? <div className="mt-2 text-[11px] text-rose-200">{pinError}</div> : null}
              {!moderatorEventsEnabled ? (
                <div className="mt-2 text-[11px] text-amber-200">Moderator events disabled by daemon caps.</div>
              ) : null}
              {props.session.id.trim().length === 0 ? (
                <div className="mt-2 text-[11px] text-amber-200">Set a session id to read events.</div>
              ) : null}
              <div className="mt-2 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/70">
                {moderatorEventsFiltered.length === 0 ? (
                  <div className="text-white/40">
                    {moderatorEventsList.length === 0 ? "No events loaded yet." : "No events match the filter."}
                  </div>
                ) : (
                  moderatorEventsFiltered.map((event, idx) => {
                    const type = typeof event?.type === "string" ? event.type : "event";
                    const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
                    const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
                    const actorId = typeof actor?.id === "string" ? actor.id : "";
                    const summary = formatModeratorEventSummary(event);
                    const key = `${type}-${event?.ts_unix_ms ?? "0"}-${idx}`;
                    const isPinned = !!moderatorPinnedEvents[key];
                    const isExpanded = moderatorEventsExpanded[key] === true;
                    return (
                      <div key={key} className="border-b border-white/5 py-1 last:border-b-0">
                        <div className="flex flex-wrap items-center justify-between gap-2">
                          <div className="text-white/80">
                            {type}
                            {actorId ? ` · ${actorId}` : ""}
                          </div>
                          <div className="flex items-center gap-2 text-white/40">
                            <span>{ts}</span>
                            {summary ? (
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopy("summary", summary)}
                              >
                                Copy summary
                              </button>
                            ) : null}
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() => void handleCopy("JSON", JSON.stringify(event, null, 2))}
                            >
                              Copy JSON
                            </button>
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() =>
                                updateModeratorPinnedEvents((prev) => {
                                  const next = { ...prev };
                                  if (next[key]) {
                                    delete next[key];
                                  } else {
                                    next[key] = event;
                                  }
                                  return next;
                                })
                              }
                            >
                              {isPinned ? "Unpin" : "Pin"}
                            </button>
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() =>
                                setModeratorEventsExpanded((prev) => ({
                                  ...prev,
                                  [key]: !isExpanded,
                                }))
                              }
                            >
                              {isExpanded ? "Hide JSON" : "Show JSON"}
                            </button>
                          </div>
                        </div>
                        {summary ? <div className="text-white/60">{summary}</div> : null}
                        {isExpanded ? (
                          <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                            {JSON.stringify(event, null, 2)}
                          </pre>
                        ) : null}
                      </div>
                    );
                  })
                )}
              </div>
            </div>
            <div className="text-[11px] text-white/50">
              Moderator directives/tasks are stored as client events. Assignees and scope are advisory hints; empty assignees
              broadcast to all listening agents.
            </div>
          </div>
        </div>

        <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <SectionHeader
            title="Client"
            action={
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                onClick={() => props.daemonConfig.refresh()}
                type="button"
                disabled={props.daemonConfig.isFetching}
              >
                Refresh config
              </button>
            }
          />
          <div className="mt-2 grid gap-2 text-[11px] text-white/70">
            <ToggleRow label="Allow audio autoplay" checked={client.allowAutoplay} onChange={client.setAllowAutoplay} />
            <ToggleRow label="Allow client RPCs" checked={client.allowClientRpcs} onChange={client.setAllowClientRpcs} />
            <ToggleRow
              label="Allow client RPC side effects"
              checked={client.allowClientEffects}
              onChange={client.setAllowClientEffects}
              disabled={!client.allowClientRpcs}
            />
            <ToggleRow
              label="Allow unsafe page eval"
              checked={client.allowUnsafePageEval}
              onChange={client.setAllowUnsafePageEval}
            />
          </div>
        </div>

        <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <SectionHeader
            title="Model / Provider"
            action={
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => {
                  run.setBaseUrl(daemonDefaults?.base_url || run.baseUrl);
                  run.setModel(daemonDefaults?.model || run.model);
                  run.setSummaryModel(daemonDefaults?.summary_model || "");
                  run.setSummaryMaxChars(
                    typeof daemonDefaults?.summary_max_chars === "number"
                      ? String(daemonDefaults?.summary_max_chars)
                      : run.summaryMaxChars,
                  );
                  run.setTimeoutMs(
                    typeof daemonDefaults?.timeout_ms === "number" ? String(daemonDefaults?.timeout_ms) : run.timeoutMs,
                  );
                }}
                title="Copy daemon defaults into local fields"
              >
                Use daemon defaults
              </button>
            }
          />
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={run.profileOverridesEnabled}
                onChange={(e) => run.setProfileOverridesEnabled(e.target.checked)}
              />
              <span>Profile-specific run settings</span>
            </label>
            <span className="text-white/40">Applies to {connection.profileName}</span>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => run.copyProfileOverridesFromGlobal()}
              disabled={!run.profileOverridesEnabled}
              title="Copy global run settings into this profile"
            >
              Sync from global
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => run.clearProfileOverrides()}
              disabled={!run.profileOverridesEnabled}
              title="Disable and clear profile overrides"
            >
              Revert to global
            </button>
          </div>
          <div className="mt-3 grid gap-3 text-[11px] text-white/70">
            <div>
              <FieldLabel>Base URL</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.baseUrl}
                onChange={(e) => run.setBaseUrl(e.target.value)}
              />
              <div className="mt-1 text-white/50">Active: {baseUrlLabel || "(empty)"}</div>
            </div>
            <div>
              <FieldLabel>Model</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.model}
                onChange={(e) => run.setModel(e.target.value)}
              />
            </div>
            <div>
              <FieldLabel>Summary model (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.summaryModel}
                onChange={(e) => run.setSummaryModel(e.target.value)}
                placeholder="Leave blank to disable summaries"
              />
            </div>
            <div>
              <FieldLabel>Summary max chars</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.summaryMaxChars}
                onChange={(e) => run.setSummaryMaxChars(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>API key (local)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.apiKey}
                onChange={(e) => run.setApiKey(e.target.value)}
                placeholder="Stored in browser storage"
              />
            </div>
            <div>
              <FieldLabel>Proxy URL</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.proxyUrl}
                onChange={(e) => run.setProxyUrl(e.target.value)}
                placeholder="e.g. http://localhost:8120"
              />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Timeout (ms)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.timeoutMs}
                  onChange={(e) => run.setTimeoutMs(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Max capture bytes</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxCaptureBytes}
                  onChange={(e) => run.setMaxCaptureBytes(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <ToggleRow label="Stream assistant" checked={run.streamAssistant} onChange={run.setStreamAssistant} />
              <ToggleRow label="Trace" checked={run.trace} onChange={run.setTrace} />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <ToggleRow label="YOLO (no tool restrictions)" checked={run.yolo} onChange={run.setYolo} />
              <ToggleRow label="Verbose" checked={run.verbose} onChange={run.setVerbose} />
              <ToggleRow label="Async run" checked={run.useAsync} onChange={run.setUseAsync} disabled={!jobsEnabled} />
              <ToggleRow
                label="Show debug in conversation"
                checked={client.showDebugInConversation}
                onChange={client.setShowDebugInConversation}
              />
            </div>
            {!jobsEnabled ? (
              <div className="text-[11px] text-amber-200">Async run disabled by daemon caps.</div>
            ) : null}
          </div>
        </div>

        <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <summary className="cursor-pointer text-xs font-semibold text-white/70">Run limits</summary>
          <div className="mt-3 grid gap-3 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Max steps</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxSteps}
                  onChange={(e) => run.setMaxSteps(e.target.value)}
                  placeholder="blank = daemon default"
                />
              </div>
              <div>
                <FieldLabel>Max repeated tool calls</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxRepeatedToolCalls}
                  onChange={(e) => run.setMaxRepeatedToolCalls(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Max tool calls total</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxToolCallsTotal}
                  onChange={(e) => run.setMaxToolCallsTotal(e.target.value)}
                  placeholder="blank = daemon default"
                />
              </div>
              <div>
                <FieldLabel>Max tool calls per tool</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxToolCallsPerTool}
                  onChange={(e) => run.setMaxToolCallsPerTool(e.target.value)}
                  placeholder="blank = daemon default"
                />
              </div>
            </div>
            <div>
              <FieldLabel>Tool call limits</FieldLabel>
              <textarea
                className="mt-1 min-h-[90px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.toolCallLimits}
                onChange={(e) => run.setToolCallLimits(e.target.value)}
                placeholder="tool=max_calls (comma or newline separated) or JSON list"
              />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Max chars</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.maxChars}
                  onChange={(e) => run.setMaxChars(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Keep last</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.keepLast}
                  onChange={(e) => run.setKeepLast(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
          </div>
        </details>

        <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <summary className="cursor-pointer text-xs font-semibold text-white/70">Memory context</summary>
          <div className="mt-3 grid gap-3 text-[11px] text-white/70">
            <div>
              <FieldLabel>Context mode</FieldLabel>
              <select
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memoryContextMode}
                onChange={(e) => run.setMemoryContextMode(e.target.value)}
              >
                <option value="files">files (read memory/*.md)</option>
                <option value="index">index (progressive file index)</option>
                <option value="search">search (ranked snippets)</option>
                <option value="salience">salience (ranked recency/importance)</option>
              </select>
              <div className="mt-1 text-[11px] text-white/40">
                Applies only when tools=host; index shows file size/line/token estimates; search defaults to the prompt if no query is provided.
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <ToggleRow label="Include structured" checked={run.memoryIncludeStructured} onChange={run.setMemoryIncludeStructured} />
              <ToggleRow label="Include core" checked={run.memoryIncludeCore} onChange={run.setMemoryIncludeCore} />
              <ToggleRow label="Include daily" checked={run.memoryIncludeDaily} onChange={run.setMemoryIncludeDaily} />
              <ToggleRow label="Include session" checked={run.memoryIncludeSession} onChange={run.setMemoryIncludeSession} />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Daily days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.memoryDailyDays}
                  onChange={(e) => run.setMemoryDailyDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Total cap (bytes)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.memoryTotalCap}
                  onChange={(e) => run.setMemoryTotalCap(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div>
              <FieldLabel>Search query (optional)</FieldLabel>
              <textarea
                className="mt-1 min-h-[70px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memorySearchQuery}
                onChange={(e) => run.setMemorySearchQuery(e.target.value)}
                placeholder="Leave blank to use the current prompt"
              />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <ToggleRow label="Use index (FTS)" checked={run.memorySearchUseIndex} onChange={run.setMemorySearchUseIndex} />
              <ToggleRow
                label="Case sensitive"
                checked={run.memorySearchCaseSensitive}
                onChange={run.setMemorySearchCaseSensitive}
              />
              <ToggleRow
                label="Fallback to files"
                checked={run.memorySearchFallbackToFiles}
                onChange={run.setMemorySearchFallbackToFiles}
              />
            </div>
            <div>
              <FieldLabel>Order</FieldLabel>
              <select
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memorySearchOrder}
                onChange={(e) => run.setMemorySearchOrder(e.target.value)}
              >
                <option value="ranked">Ranked (relevance)</option>
                <option value="newest">Newest first</option>
                <option value="oldest">Oldest first</option>
              </select>
            </div>
            <div className="grid grid-cols-3 gap-3">
              <div>
                <FieldLabel>Max results</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.memorySearchMaxResults}
                  onChange={(e) => run.setMemorySearchMaxResults(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Max snippet chars</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.memorySearchMaxSnippetChars}
                  onChange={(e) => run.setMemorySearchMaxSnippetChars(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Context lines</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.memorySearchContextLines}
                  onChange={(e) => run.setMemorySearchContextLines(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
          </div>
        </details>

        <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <summary className="cursor-pointer text-xs font-semibold text-white/70">OpenRouter model picker</summary>
          <div className="mt-3 grid gap-3 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Min total ($/1M)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.orMinTotal}
                  onChange={(e) => run.setOrMinTotal(e.target.value)}
                />
              </div>
              <div>
                <FieldLabel>Max total ($/1M)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.orMaxTotal}
                  onChange={(e) => run.setOrMaxTotal(e.target.value)}
                />
              </div>
              <div>
                <FieldLabel>Limit</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={run.orLimit}
                  onChange={(e) => run.setOrLimit(e.target.value)}
                />
              </div>
              <div className="grid gap-2">
                <ToggleRow
                  label="Require multimodal"
                  checked={run.orRequireMultimodal}
                  onChange={run.setOrRequireMultimodal}
                />
                <ToggleRow label="Require tools" checked={run.orRequireTools} onChange={run.setOrRequireTools} />
              </div>
            </div>

            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={fetchOpenRouterModels.isPending}
              onClick={() => fetchOpenRouterModels.mutate()}
            >
              {fetchOpenRouterModels.isPending ? "Loading…" : "Fetch models"}
            </button>
            {fetchOpenRouterModels.isError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
                Fetch failed: {String(fetchOpenRouterModels.error)}
              </div>
            ) : null}

            {openrouterModels ? (
              <div className="rounded-md border border-white/10 bg-black/30 p-3">
                <div className="text-white/60">
                  {openrouterModels.ok ? (
                    <>
                      <span className="text-emerald-200">ok</span> · count={openrouterModels.count ?? 0} ·
                      cached={String(openrouterModels.cached ?? false)}
                    </>
                  ) : (
                    <span className="text-rose-200">error</span>
                  )}
                </div>
                {openrouterModels.error ? (
                  <div className="mt-2 text-rose-200">{String(openrouterModels.error)}</div>
                ) : null}
                {openrouterModels.recommended_model ? (
                  <div className="mt-2">
                    Recommended: <code className="text-white/80">{openrouterModels.recommended_model}</code>
                  </div>
                ) : null}
                {Array.isArray(openrouterModels.models) && openrouterModels.models.length > 0 ? (
                  <div className="mt-3 max-h-48 overflow-auto rounded-md border border-white/10">
                    {openrouterModels.models.slice(0, 50).map((m: any) => {
                      const id = typeof m?.id === "string" ? m.id : "";
                      if (!id) return null;
                      const total = typeof m?.total_usd_per_million === "number" ? m.total_usd_per_million : null;
                      const ctx = typeof m?.context_length === "number" ? m.context_length : null;
                      return (
                        <button
                          key={id}
                          type="button"
                          className="flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5"
                          onClick={() => {
                            run.setModel(id);
                            run.setBaseUrl("https://openrouter.ai/api/v1");
                          }}
                        >
                          <span className="font-mono text-white/80">{id}</span>
                          <span className="text-white/50">
                            {ctx ? `ctx=${ctx}` : ""}
                            {total !== null ? ` · $${total.toFixed(3)}/1M` : ""}
                          </span>
                        </button>
                      );
                    })}
                  </div>
                ) : null}
              </div>
            ) : null}
          </div>
        </details>

        <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <SectionHeader
            title="Daemon defaults (persisted)"
            action={
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => props.daemonConfig.refresh()}
                type="button"
                disabled={props.daemonConfig.isFetching}
              >
                Refresh
              </button>
            }
          />
          <div className="mt-2 text-[11px] text-white/60">
            Saves to daemon state (server-side). This avoids keeping provider keys in browser storage.
          </div>
          <div className="mt-3 grid gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.updateDaemonDefaults.pending}
              onClick={props.updateDaemonDefaults.saveDefaults}
            >
              Save model/base_url/proxy/timeout to daemon
            </button>
            <div className="flex items-center gap-2">
              <button
                className="flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={props.updateDaemonDefaults.pending}
                onClick={props.updateDaemonDefaults.saveApiKey}
                title="Stores the provider key on the daemon host (in state_dir/runtime_secrets.env)."
              >
                Save API key to daemon (current provider)
              </button>
              <button
                className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                type="button"
                disabled={props.updateDaemonDefaults.pending}
                onClick={props.updateDaemonDefaults.clearApiKey}
              >
                Clear key
              </button>
            </div>
            {props.updateDaemonDefaults.error ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                Save failed: {props.updateDaemonDefaults.error}
              </div>
            ) : null}
            {props.updateDaemonDefaults.success ? (
              <div className="rounded-md border border-emerald-500/30 bg-emerald-500/10 px-3 py-2 text-xs text-emerald-100">
                Saved.
              </div>
            ) : null}
          </div>
          {daemonDefaults ? (
            <div className="mt-3 grid gap-1 text-[11px] text-white/60">
              <div>base_url: <code className="text-white/70">{daemonDefaults.base_url || "(unset)"}</code></div>
              <div>model: <code className="text-white/70">{daemonDefaults.model || "(unset)"}</code></div>
              <div>summary_model: <code className="text-white/70">{daemonDefaults.summary_model || "(unset)"}</code></div>
              <div>summary_max_chars: <code className="text-white/70">{String(daemonDefaults.summary_max_chars ?? "(unset)")}</code></div>
              <div>timeout_ms: <code className="text-white/70">{String(daemonDefaults.timeout_ms ?? "(unset)")}</code></div>
              <div>proxy_url_set: <code className="text-white/70">{String(daemonDefaults.proxy_url_set ?? false)}</code></div>
              <div>api_key_set: <code className="text-white/70">{String(daemonDefaults.api_key_set ?? false)}</code></div>
              <div>
                max_steps_default: <code className="text-white/70">{String(daemonDefaults.max_steps_default ?? "(unset)")}</code>
              </div>
              <div>
                max_tool_calls_total_default: <code className="text-white/70">{String(daemonDefaults.max_tool_calls_total_default ?? "(unset)")}</code>
              </div>
              <div>
                max_tool_calls_per_tool_default: <code className="text-white/70">{String(daemonDefaults.max_tool_calls_per_tool_default ?? "(unset)")}</code>
              </div>
              <div>
                max_tool_call_args_chars_default: <code className="text-white/70">{String(daemonDefaults.max_tool_call_args_chars_default ?? "(unset)")}</code>
              </div>
            </div>
          ) : null}
        </div>

        <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <summary className="cursor-pointer text-xs font-semibold text-white/70">Capabilities</summary>
          <div className="mt-2 grid gap-2 text-[11px] text-white/70">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <div>
                service:{" "}
                <code className="text-white/70">{capsData?.service || "(unknown)"}</code>{" "}
                · version: <code className="text-white/70">{capsData?.version || "(unknown)"}</code>
              </div>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => props.caps.refresh()}
                disabled={props.caps.isFetching}
              >
                {props.caps.isFetching ? "Loading…" : "Refresh"}
              </button>
            </div>
            <div>
              source: <code className="text-white/70">{props.caps.source}</code>
              {capsAge ? <span className="text-white/50"> · age {capsAge}</span> : null}
            </div>
            {props.caps.error ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
                caps fetch failed: {String(props.caps.error)}
              </div>
            ) : null}
            {capsJson ? (
              <pre className="max-h-64 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[10px] text-white/60">
                {capsJson}
              </pre>
            ) : null}
          </div>
        </details>

        <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
          <SectionHeader
            title="Diagnostics"
            action={
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={diagnostics.isFetching || diagnosticsProviders.isFetching}
                onClick={() => {
                  void diagnostics.refetch();
                  void diagnosticsProviders.refetch();
                }}
              >
                Refresh
              </button>
            }
          />
          <div className="mt-2 grid gap-2 text-[11px] text-white/70">
            <div>
              ready:{" "}
              <span className={diag?.ready ? "text-emerald-200" : "text-rose-200"}>
                {typeof diag?.ready === "boolean" ? String(diag.ready) : "unknown"}
              </span>
              {diag?.uptime_ms ? (
                <span className="text-white/50"> · uptime {formatDuration(diag.uptime_ms)}</span>
              ) : null}
            </div>
            <div>
              db:{" "}
              <code className="text-white/70">
                {typeof diag?.db?.path === "string" ? diag.db.path : "(unknown)"}
              </code>
              {typeof diag?.db?.size_bytes === "number" ? (
                <span className="text-white/50"> · {formatBytes(diag.db.size_bytes)}</span>
              ) : null}
            </div>
            {diag?.jobs && typeof diag.jobs === "object" ? (
              <div>
                jobs total:{" "}
                <code className="text-white/70">{String((diag.jobs as any).total ?? "(unknown)")}</code>
              </div>
            ) : null}
            {diag?.workflows && typeof diag.workflows === "object" ? (
              <div>
                workflows queued:{" "}
                <code className="text-white/70">
                  {String((diag.workflows as any).tasks_queued_ready ?? "(unknown)")}
                </code>
              </div>
            ) : null}
            {Array.isArray(diag?.warnings) && diag?.warnings.length > 0 ? (
              <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-amber-100">
                {diag.warnings.join("; ")}
              </div>
            ) : null}
            {diagnostics.isError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
                diagnostics failed: {String(diagnostics.error)}
              </div>
            ) : null}
            {diagnosticsProviders.isError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
                provider status failed: {String(diagnosticsProviders.error)}
              </div>
            ) : null}
          </div>

          <div className="mt-3">
            <div className="text-[11px] font-semibold text-white/60">Providers</div>
            <div className="mt-2 rounded-md border border-white/10 bg-black/20">
              {["deepseek", "moonshot", "openrouter", "openai"].map((name) => {
                const p = providerEntries[name] || {};
                const keyPresent = p.key_present === true;
                const source = p.source && typeof p.source === "object"
                  ? `${p.source.kind ?? "source"}:${p.source.label ?? "unknown"}`
                  : "";
                const baseUrl = typeof p.base_url === "string" ? p.base_url : "";
                const model = typeof p.model === "string" ? p.model : "";
                const modelDefault = typeof p.model_default === "string" ? p.model_default : "";
                const warning = typeof p.warning === "string" ? p.warning : "";
                return (
                  <div key={name} className="border-t border-white/5 px-3 py-2 text-[11px] text-white/70 first:border-t-0">
                    <div className="flex items-center justify-between gap-2">
                      <span className="font-mono text-white/80">{name}</span>
                      <span className={keyPresent ? "text-emerald-200" : "text-rose-200"}>
                        key={keyPresent ? "present" : "missing"}
                      </span>
                    </div>
                    {source ? <div className="text-white/50">source: {source}</div> : null}
                    {baseUrl ? <div className="text-white/50">base_url: {baseUrl}</div> : null}
                    {model ? <div className="text-white/50">model: {model}</div> : null}
                    {modelDefault ? <div className="text-white/40">default: {modelDefault}</div> : null}
                    {warning ? <div className="text-amber-200/80">warning: {warning}</div> : null}
                  </div>
                );
              })}
            </div>
          </div>

          <div className="mt-3 flex flex-wrap items-center gap-2 text-[11px] text-white/70">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void runProviderTest("deepseek")}
              disabled={!deepseekKeyPresent}
              title={deepseekKeyPresent ? "Run DeepSeek provider test" : "DeepSeek key missing"}
            >
              Test DeepSeek
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void runProviderTest("moonshot")}
              disabled={!moonshotKeyPresent}
              title={moonshotKeyPresent ? "Run Moonshot provider test" : "Moonshot key missing"}
            >
              Test Moonshot
            </button>
            <span>
              DeepSeek: {providerStatus("deepseek")} · Moonshot: {providerStatus("moonshot")}
            </span>
          </div>
          {Object.entries(providerTests).map(([name, entry]) => {
            if (!entry || !entry.error) return null;
            return (
              <div
                key={`provider-error-${name}`}
                className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200"
              >
                {name} test error: {String(entry.error)}
              </div>
            );
          })}
          <div className="mt-2 text-[11px] text-white/50">
            Provider tests use the diagnostics endpoint and will not persist keys in the browser.
          </div>
        </div>

        <div className="mt-4">
          <div className="text-xs font-semibold text-white/70">Sessions</div>
          <div className="mt-2 flex flex-wrap gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
              onClick={() => props.session.refresh()}
              type="button"
            >
              Refresh
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              onClick={() => props.session.newSession()}
              type="button"
              data-testid="new-session"
              disabled={props.session.newSessionPending}
            >
              New session
            </button>
            <button
              className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
              onClick={() => {
                const ids = props.session.sessions ?? [];
                const n = ids.length;
                if (n === 0) return;
                if (!clearAllArmed) {
                  setClearAllArmed(true);
                  try {
                    if (clearAllArmTimeoutRef.current) window.clearTimeout(clearAllArmTimeoutRef.current);
                  } catch {
                    // ignore
                  }
                  clearAllArmTimeoutRef.current = window.setTimeout(() => setClearAllArmed(false), 8000);
                  return;
                }
                setClearAllArmed(false);
                props.session.clearAll();
              }}
              type="button"
              disabled={props.session.clearAllPending}
              title="Danger: deletes all sessions on the daemon."
            >
              {clearAllArmed ? `Confirm clear all (${props.session.sessions.length})` : "Clear all"}
            </button>
            {clearAllArmed ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => setClearAllArmed(false)}
              >
                Cancel
              </button>
            ) : null}
          </div>
          <div className="mt-2 max-h-64 overflow-auto rounded-md border border-white/10 bg-black/20">
            {(props.session.sessions ?? []).map((sid) => {
              const selected = sid === props.session.id;
              return (
                <div
                  key={sid}
                  className={`flex items-center justify-between gap-2 px-3 py-2 text-xs ${selected ? "bg-white/10" : ""}`}
                >
                  <button className="flex-1 text-left hover:underline" onClick={() => props.session.setId(sid)} type="button">
                    {sid}
                  </button>
                  <button
                    className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                    type="button"
                    disabled={props.session.deletePending}
                    onClick={() => {
                      if (!confirm(`Delete session '${sid}'?`)) return;
                      props.session.deleteSession(sid);
                    }}
                  >
                    Delete
                  </button>
                </div>
              );
            })}
          </div>
          {props.session.deleteError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
              Delete failed: {props.session.deleteError}
            </div>
          ) : null}
          {props.session.clearAllError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
              Clear all failed: {props.session.clearAllError}
            </div>
          ) : null}
        </div>
      </div>
    </div>
  );
}
