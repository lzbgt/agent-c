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
  apiPostSandboxMountValidate,
} from "../api";
import type { ModeratorEvent } from "../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../hooks/useUiSettings";
import useLocalStorageState from "../hooks/useLocalStorageState";
import SettingsConnectionSection from "./settings/SettingsConnectionSection";
import SettingsDiagnosticsSection from "./settings/SettingsDiagnosticsSection";
import SettingsExecutionSection from "./settings/SettingsExecutionSection";
import SettingsModeratorSection from "./settings/SettingsModeratorSection";
import SettingsSessionsSection from "./settings/SettingsSessionsSection";

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
  const [connectorStaleMinutes, setConnectorStaleMinutes] = useLocalStorageState<string>(
    "agentui.connectorStaleMinutes",
    "10",
  );
  const [sandboxMountHostPath, setSandboxMountHostPath] = useLocalStorageState<string>(
    "agentui.sandboxMountHostPath",
    "",
  );
  const [sandboxMountContainerPath, setSandboxMountContainerPath] = useLocalStorageState<string>(
    "agentui.sandboxMountContainerPath",
    "/workspace/extra/example",
  );
  const [sandboxMountContainerPrefix, setSandboxMountContainerPrefix] = useLocalStorageState<string>(
    "agentui.sandboxMountContainerPrefix",
    "/workspace/extra",
  );
  const [sandboxMountIsMain, setSandboxMountIsMain] = useLocalStorageState<boolean>(
    "agentui.sandboxMountIsMain",
    true,
  );
  const [sandboxMountResult, setSandboxMountResult] = React.useState<any | null>(null);
  const [sandboxMountError, setSandboxMountError] = React.useState<string | null>(null);
  const [copyNotice, setCopyNotice] = React.useState<string | null>(null);
  const [pinNotice, setPinNotice] = React.useState<string | null>(null);
  const [pinError, setPinError] = React.useState<string | null>(null);
  const [pinnedCompareA, setPinnedCompareA] = React.useState<string>("");
  const [pinnedCompareB, setPinnedCompareB] = React.useState<string>("");
  const [pinnedCompareDiffOnly, setPinnedCompareDiffOnly] = React.useState<boolean>(false);
  const copyNoticeTimeoutRef = React.useRef<number>(0);
  const pinNoticeTimeoutRef = React.useRef<number>(0);
  const pinImportRef = React.useRef<HTMLInputElement>(null);
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
  }, [connection.brokerBase, connection.brokerAuthToken, connection.brokerCookieAuth]);

  React.useEffect(() => {
    setBrokerDeployments(null);
    setBrokerDeploymentsError(null);
    setBrokerDeploymentsDefaultId(null);
  }, [connection.brokerBase, connection.brokerAuthToken, connection.brokerCookieAuth, connection.brokerAgentId]);

  const brokerAuthReady = connection.brokerCookieAuth || String(connection.brokerAuthToken || "").trim().length > 0;

  const listBrokerAgents = React.useCallback(async () => {
    setBrokerAgentsError(null);
    setBrokerAgentsBusy(true);
    try {
      const bb = String(connection.brokerBase || "").trim().replace(/\/+$/, "");
      const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
      const r = await apiBrokerListAgents(withScheme, connection.daemonAuth);
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
      const r = await apiBrokerListDeployments(withScheme, agentId, connection.daemonAuth);
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
  const sandboxMountValidate = useMutation({
    mutationFn: async () => {
      const hostPath = String(sandboxMountHostPath || "").trim();
      const containerPath = String(sandboxMountContainerPath || "").trim();
      if (!hostPath) {
        throw new Error("host_path is required");
      }
      if (!containerPath) {
        throw new Error("container_path is required");
      }
      return apiPostSandboxMountValidate(
        connection.effectiveBase,
        {
          host_path: hostPath,
          container_path: containerPath,
          container_prefix: String(sandboxMountContainerPrefix || "").trim() || undefined,
          is_main: sandboxMountIsMain,
        },
        connection.daemonAuth,
      );
    },
    onSuccess: (resp) => {
      setSandboxMountResult(resp);
      setSandboxMountError(null);
    },
    onError: (err) => {
      setSandboxMountResult(null);
      setSandboxMountError(String(err));
    },
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
      let ok = false;
      if (text) {
        try {
          if (navigator.clipboard && typeof navigator.clipboard.writeText === "function") {
            await navigator.clipboard.writeText(text);
            ok = true;
          }
        } catch {
          // fallback below
        }
        if (!ok) {
          try {
            const el = document.createElement("textarea");
            el.value = text;
            el.setAttribute("readonly", "true");
            el.style.position = "absolute";
            el.style.left = "-9999px";
            document.body.appendChild(el);
            el.select();
            ok = document.execCommand("copy");
            document.body.removeChild(el);
          } catch {
            ok = false;
          }
        }
      }
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
    if (!props.open) return;
    const onKey = (event: KeyboardEvent) => {
      if (!event.ctrlKey || !event.shiftKey) return;
      const target = event.target as HTMLElement | null;
      if (target) {
        const tag = target.tagName ? target.tagName.toLowerCase() : "";
        if (tag === "input" || tag === "textarea" || tag === "select" || target.isContentEditable) {
          return;
        }
      }
      if (event.code === "KeyS") {
        if (pinnedCompareA && pinnedCompareB) {
          event.preventDefault();
          setPinnedCompareA(pinnedCompareB);
          setPinnedCompareB(pinnedCompareA);
        }
      } else if (event.code === "KeyD") {
        event.preventDefault();
        setPinnedCompareDiffOnly((prev) => !prev);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => {
      window.removeEventListener("keydown", onKey);
    };
  }, [pinnedCompareA, pinnedCompareB, props.open]);
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

        <SettingsConnectionSection
          connection={connection}
          run={run}
          client={client}
          session={props.session}
          serverPrefsCanSync={serverPrefsCanSync}
          serverPrefsTarget={serverPrefsTarget}
          serverPrefsStatusLabel={serverPrefsStatusLabel}
          serverPrefsAutoNote={serverPrefsAutoNote}
          brokerAuthReady={brokerAuthReady}
          brokerAgentsBusy={brokerAgentsBusy}
          brokerAgentsError={brokerAgentsError}
          brokerAgents={brokerAgents}
          brokerDeploymentsBusy={brokerDeploymentsBusy}
          brokerDeploymentsError={brokerDeploymentsError}
          brokerDeployments={brokerDeployments}
          brokerDeploymentsDefaultId={brokerDeploymentsDefaultId}
          listBrokerAgents={listBrokerAgents}
          listBrokerDeployments={listBrokerDeployments}
          automationProfiles={automationProfiles}
          automationDefault={automationDefault}
          automationOverrideAllowed={automationOverrideAllowed}
        />

        <SettingsModeratorSection
          connection={connection}
          client={client}
          sessionId={props.session.id}
          moderatorDirective={moderatorDirective}
          setModeratorDirective={setModeratorDirective}
          moderatorDirectiveScope={moderatorDirectiveScope}
          setModeratorDirectiveScope={setModeratorDirectiveScope}
          moderatorDirectiveAssignees={moderatorDirectiveAssignees}
          setModeratorDirectiveAssignees={setModeratorDirectiveAssignees}
          moderatorDirectivePick={moderatorDirectivePick}
          setModeratorDirectivePick={setModeratorDirectivePick}
          moderatorTaskTitle={moderatorTaskTitle}
          setModeratorTaskTitle={setModeratorTaskTitle}
          moderatorTaskDetail={moderatorTaskDetail}
          setModeratorTaskDetail={setModeratorTaskDetail}
          moderatorTaskAssignees={moderatorTaskAssignees}
          setModeratorTaskAssignees={setModeratorTaskAssignees}
          moderatorTaskPick={moderatorTaskPick}
          setModeratorTaskPick={setModeratorTaskPick}
          moderatorAppendToSession={moderatorAppendToSession}
          setModeratorAppendToSession={setModeratorAppendToSession}
          moderatorBusy={moderatorBusy}
          moderatorError={moderatorError}
          moderatorSuccess={moderatorSuccess}
          moderatorEventsAuto={moderatorEventsAuto}
          setModeratorEventsAuto={setModeratorEventsAuto}
          moderatorEventsMaxBytes={moderatorEventsMaxBytes}
          setModeratorEventsMaxBytes={setModeratorEventsMaxBytes}
          moderatorEventsIncludeDirectives={moderatorEventsIncludeDirectives}
          setModeratorEventsIncludeDirectives={setModeratorEventsIncludeDirectives}
          moderatorEventsIncludeTasks={moderatorEventsIncludeTasks}
          setModeratorEventsIncludeTasks={setModeratorEventsIncludeTasks}
          moderatorEventsFilter={moderatorEventsFilter}
          setModeratorEventsFilter={setModeratorEventsFilter}
          moderatorEventsExpanded={moderatorEventsExpanded}
          setModeratorEventsExpanded={setModeratorEventsExpanded}
          brokerAgentOptions={brokerAgentOptions}
          brokerAgentsBusy={brokerAgentsBusy}
          listBrokerAgents={listBrokerAgents}
          moderatorRolePresets={moderatorRolePresets}
          addDirectiveAssignee={addDirectiveAssignee}
          addTaskAssignee={addTaskAssignee}
          applyRuntimeMemberTaskTemplate={applyRuntimeMemberTaskTemplate}
          publishModeratorDirective={publishModeratorDirective}
          publishModeratorTask={publishModeratorTask}
          moderatorEventsEnabled={moderatorEventsEnabled}
          moderatorEventsRefetch={() => void moderatorEvents.refetch()}
          moderatorEventsFetching={moderatorEvents.isFetching}
          moderatorEventsError={moderatorEventsError}
          moderatorEventsList={moderatorEventsList}
          moderatorEventsFiltered={moderatorEventsFiltered}
          moderatorPinnedEvents={moderatorPinnedEvents}
          moderatorPinnedEntries={moderatorPinnedEntries}
          updateModeratorPinnedEvents={updateModeratorPinnedEvents}
          pinImportRef={pinImportRef}
          showPinNotice={showPinNotice}
          handleCopy={handleCopy}
          pinnedCompareOptions={pinnedCompareOptions}
          pinnedCompareA={pinnedCompareA}
          setPinnedCompareA={setPinnedCompareA}
          pinnedCompareB={pinnedCompareB}
          setPinnedCompareB={setPinnedCompareB}
          pinnedCompareDiffOnly={pinnedCompareDiffOnly}
          setPinnedCompareDiffOnly={setPinnedCompareDiffOnly}
          copyNotice={copyNotice}
          pinNotice={pinNotice}
          pinError={pinError}
          moderatorDirectivesEnabled={moderatorDirectivesEnabled}
          moderatorTasksEnabled={moderatorTasksEnabled}
        />

        <SettingsExecutionSection
          connection={connection}
          run={run}
          client={client}
          daemonConfig={props.daemonConfig}
          updateDaemonDefaults={props.updateDaemonDefaults}
          daemonDefaults={daemonDefaults}
          caps={props.caps}
          capsAge={capsAge}
          capsJson={capsJson}
          connectorStaleMinutes={connectorStaleMinutes}
          setConnectorStaleMinutes={setConnectorStaleMinutes}
          jobsEnabled={jobsEnabled}
          baseUrlLabel={baseUrlLabel}
          fetchOpenRouterModelsPending={fetchOpenRouterModels.isPending}
          fetchOpenRouterModelsError={fetchOpenRouterModels.isError ? String(fetchOpenRouterModels.error) : null}
          onFetchOpenRouterModels={() => fetchOpenRouterModels.mutate()}
          openrouterModels={openrouterModels}
        />

        <SettingsDiagnosticsSection
          diagnostics={diag}
          diagnosticsProviders={diagnosticsProviders.data}
          diagnosticsFetching={diagnostics.isFetching}
          diagnosticsProvidersFetching={diagnosticsProviders.isFetching}
          diagnosticsError={diagnostics.isError ? String(diagnostics.error) : null}
          diagnosticsProvidersError={diagnosticsProviders.isError ? String(diagnosticsProviders.error) : null}
          onRefresh={() => {
            void diagnostics.refetch();
            void diagnosticsProviders.refetch();
          }}
          sandboxMountHostPath={String(sandboxMountHostPath || "")}
          setSandboxMountHostPath={setSandboxMountHostPath}
          sandboxMountContainerPath={String(sandboxMountContainerPath || "")}
          setSandboxMountContainerPath={setSandboxMountContainerPath}
          sandboxMountContainerPrefix={String(sandboxMountContainerPrefix || "")}
          setSandboxMountContainerPrefix={setSandboxMountContainerPrefix}
          sandboxMountIsMain={sandboxMountIsMain}
          setSandboxMountIsMain={setSandboxMountIsMain}
          sandboxMountPending={sandboxMountValidate.isPending}
          sandboxMountError={sandboxMountError}
          sandboxMountResult={sandboxMountResult}
          onValidateSandboxMount={() => sandboxMountValidate.mutate()}
          canValidateSandboxMount={!!String(connection.effectiveBase || "").trim()}
          providerTests={providerTests}
          onRunProviderTest={(provider) => void runProviderTest(provider)}
        />

        <SettingsSessionsSection
          session={props.session}
          clearAllArmed={clearAllArmed}
          setClearAllArmed={setClearAllArmed}
          clearAllArmTimeoutRef={clearAllArmTimeoutRef}
        />
      </div>
    </div>
  );
}
