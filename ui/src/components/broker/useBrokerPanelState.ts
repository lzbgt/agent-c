import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiBrokerDeleteMember,
  apiBrokerExportConnectors,
  apiBrokerGetMembers,
  apiBrokerGetMembershipAudit,
  apiBrokerListAgents,
  apiBrokerListConnectors,
  apiBrokerListDeployments,
  apiBrokerMemoryRecapsCreateBulk,
  apiBrokerMemoryRecapsListBulk,
  apiBrokerMemoryRetentionBulk,
  apiBrokerMemorySalienceBulk,
  apiBrokerOtaStatus,
  apiBrokerOtaStatusBulk,
  apiBrokerOtaUpdate,
  apiBrokerOtaUpdateBulk,
  apiBrokerProxyJson,
  apiBrokerUpsertMember,
  type ApiAuth,
  type BrokerAgentInfo,
  type BrokerDeploymentInfo,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import {
  asFiniteNumber,
  normalizeBrokerFanoutResults,
  normalizeDeploymentId,
  type BrokerFanoutResult,
} from "./brokerPanelUtils";
import useBrokerEventsState from "./useBrokerEventsState";

const OTA_STATUS_CACHE_TTL_MS = 10 * 60 * 1000;

export function normalizeBrokerBase(raw: string) {
  const base = String(raw || "").trim();
  if (!base) return "";
  const withScheme = /^https?:\/\//i.test(base) ? base : `https://${base}`;
  return withScheme.replace(/\/+$/, "");
}

export function fmtTs(ms?: number | null) {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

type UseBrokerPanelStateArgs = {
  brokerBase: string;
  brokerAgentId: string;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
  open: boolean;
};

export default function useBrokerPanelState(args: UseBrokerPanelStateArgs) {
  const base = React.useMemo(() => normalizeBrokerBase(args.brokerBase), [args.brokerBase]);
  const agentId = String(args.brokerAgentId || "").trim();
  const authToken = args.auth?.token ? String(args.auth.token).trim() : "";
  const useCookieAuth = args.auth?.mode === "broker" && args.auth.useCookieAuth === true;
  const canQuery = base.length > 0 && (authToken.length > 0 || useCookieAuth);

  const brokerPageKey = React.useMemo(() => {
    const b = base || "default";
    const k = String(args.authKey || "").trim() || "default";
    return `agentui.brokerPage:${b}::${k}`;
  }, [args.authKey, base]);

  const [connectorStaleMinutes, setConnectorStaleMinutes] = useLocalStorageState<string>("agentui.connectorStaleMinutes", "10");
  const brokerPages = React.useMemo(
    () => [
      { id: "teams", label: "Teams" },
      { id: "agents", label: "Agents" },
      { id: "audio", label: "Audio" },
      { id: "connectors", label: "Connectors" },
      { id: "members", label: "Member list" },
      { id: "deployments", label: "Deployments + OTA" },
      { id: "memory", label: "Memory" },
      { id: "audit", label: "Membership audit" },
      { id: "events", label: "Events" },
    ],
    [],
  );
  const brokerPageIds = React.useMemo(() => new Set(brokerPages.map((page) => page.id)), [brokerPages]);
  const brokerPageDefault = React.useMemo(() => {
    if (typeof window === "undefined") return "teams";
    const stored = window.localStorage.getItem(brokerPageKey);
    if (stored && stored.trim().length > 0) return stored.trim();
    const panelOpenRaw = window.localStorage.getItem("agentui.brokerPanelOpen");
    const panelOpen = panelOpenRaw === "true" || panelOpenRaw === "1";
    return panelOpen ? "members" : "teams";
  }, [brokerPageKey]);
  const [brokerPage, setBrokerPage] = useLocalStorageState<string>(brokerPageKey, brokerPageDefault);
  React.useEffect(() => {
    if (!brokerPageIds.has(brokerPage)) setBrokerPage("teams");
  }, [brokerPage, brokerPageIds, setBrokerPage]);

  const staleMinutesValue = Number(connectorStaleMinutes);
  const effectiveStaleMinutes = Number.isFinite(staleMinutesValue) && staleMinutesValue > 0 ? staleMinutesValue : 10;
  const connectorStaleMs = effectiveStaleMinutes * 60 * 1000;

  const agentsQuery = useQuery({
    queryKey: ["brokerAgents", base, args.authKey],
    enabled: false,
    queryFn: () => apiBrokerListAgents(base, args.auth),
  });

  const connectorsQuery = useQuery({
    queryKey: ["brokerConnectors", base, args.authKey],
    enabled: false,
    queryFn: () => apiBrokerListConnectors(base, args.auth),
  });

  const membersQuery = useQuery({
    queryKey: ["brokerMembers", base, args.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerGetMembers(base, agentId, args.auth),
  });

  const deploymentsQuery = useQuery({
    queryKey: ["brokerDeployments", base, args.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerListDeployments(base, agentId, args.auth),
  });

  const auditLimitDefault = "200";
  const [auditLimit, setAuditLimit] = React.useState<string>(auditLimitDefault);
  const limitValue = React.useMemo(() => {
    const n = Number.parseInt(String(auditLimit || ""), 10);
    if (!Number.isFinite(n) || n <= 0) return 200;
    return Math.min(Math.max(n, 1), 500);
  }, [auditLimit]);

  const auditQuery = useQuery({
    queryKey: ["brokerMembershipAudit", base, args.authKey, agentId, limitValue],
    enabled: false,
    queryFn: () => apiBrokerGetMembershipAudit(base, agentId, limitValue, args.auth),
  });

  React.useEffect(() => {
    if (!args.open || !canQuery) return;
    if (!agentsQuery.data && !agentsQuery.isFetching) {
      void agentsQuery.refetch();
    }
    if (!connectorsQuery.data && !connectorsQuery.isFetching) {
      void connectorsQuery.refetch();
    }
  }, [agentsQuery, args.open, canQuery, connectorsQuery]);

  React.useEffect(() => {
    if (!args.open || !canQuery || !agentId) return;
    if (!membersQuery.data && !membersQuery.isFetching) void membersQuery.refetch();
    if (!auditQuery.data && !auditQuery.isFetching) void auditQuery.refetch();
    if (!deploymentsQuery.data && !deploymentsQuery.isFetching) void deploymentsQuery.refetch();
  }, [agentId, args.open, auditQuery, canQuery, deploymentsQuery, membersQuery]);

  const brokerEventsState = useBrokerEventsState({
    base,
    auth: args.auth,
    authKey: args.authKey,
    clientId: args.clientId,
    open: args.open,
    canQuery,
  });

  const otaStatusCacheKey = React.useMemo(() => {
    if (!base || !agentId) return "";
    return `agentd:broker:otaStatus:${base}:${agentId}`;
  }, [agentId, base]);

  const [selectedDeployments, setSelectedDeployments] = React.useState<string[]>([]);
  const [otaUrl, setOtaUrl] = React.useState<string>("");
  const [otaSha256, setOtaSha256] = React.useState<string>("");
  const [otaVersion, setOtaVersion] = React.useState<string>("");
  const [otaDrainMs, setOtaDrainMs] = React.useState<string>("15000");
  const [otaReason, setOtaReason] = React.useState<string>("");
  const [otaBusy, setOtaBusy] = React.useState<boolean>(false);
  const [otaError, setOtaError] = React.useState<string | null>(null);
  const [otaResults, setOtaResults] = React.useState<BrokerFanoutResult[] | null>(null);
  const [otaStatusBusy, setOtaStatusBusy] = React.useState<boolean>(false);
  const [otaStatusError, setOtaStatusError] = React.useState<string | null>(null);
  const [otaStatusResults, setOtaStatusResults] = React.useState<BrokerFanoutResult[] | null>(null);
  const [otaStatusCachedAt, setOtaStatusCachedAt] = React.useState<string | null>(null);

  const [retentionDryRun, setRetentionDryRun] = React.useState<boolean>(true);
  const [retentionDailyMaxDays, setRetentionDailyMaxDays] = React.useState<string>("30");
  const [retentionDailyMaxBytes, setRetentionDailyMaxBytes] = React.useState<string>("0");
  const [retentionCheckpointMaxDays, setRetentionCheckpointMaxDays] = React.useState<string>("30");
  const [retentionCheckpointMaxCount, setRetentionCheckpointMaxCount] = React.useState<string>("200");
  const [retentionStructuredDeprecateDays, setRetentionStructuredDeprecateDays] = React.useState<string>("90");
  const [retentionStructuredDeprecateMaxEntries, setRetentionStructuredDeprecateMaxEntries] = React.useState<string>("50");
  const [retentionBusy, setRetentionBusy] = React.useState<boolean>(false);
  const [retentionError, setRetentionError] = React.useState<string | null>(null);
  const [retentionResults, setRetentionResults] = React.useState<BrokerFanoutResult[] | null>(null);

  const [recapsLimit, setRecapsLimit] = React.useState<string>("20");
  const [recapsIncludeSummary, setRecapsIncludeSummary] = React.useState<boolean>(false);
  const [recapsDryRun, setRecapsDryRun] = React.useState<boolean>(true);
  const [recapsWriteFile, setRecapsWriteFile] = React.useState<boolean>(true);
  const [recapsKind, setRecapsKind] = React.useState<string>("");
  const [recapsKindFilter, setRecapsKindFilter] = React.useState<string>("");
  const [recapsModel, setRecapsModel] = React.useState<string>("");
  const [recapsSummaryMaxChars, setRecapsSummaryMaxChars] = React.useState<string>("1200");
  const [recapsDailyDays, setRecapsDailyDays] = React.useState<string>("7");
  const [recapsMaxItems, setRecapsMaxItems] = React.useState<string>("12");
  const [recapsStructuredMaxItems, setRecapsStructuredMaxItems] = React.useState<string>("6");
  const [recapsDailyMaxItems, setRecapsDailyMaxItems] = React.useState<string>("6");
  const [recapsHalfLifeDays, setRecapsHalfLifeDays] = React.useState<string>("14");
  const [recapsImportanceWeight, setRecapsImportanceWeight] = React.useState<string>("0.35");
  const [recapsIncludeStructured, setRecapsIncludeStructured] = React.useState<boolean>(true);
  const [recapsIncludeDaily, setRecapsIncludeDaily] = React.useState<boolean>(true);
  const [recapsListBusy, setRecapsListBusy] = React.useState<boolean>(false);
  const [recapsGenerateBusy, setRecapsGenerateBusy] = React.useState<boolean>(false);
  const [recapsError, setRecapsError] = React.useState<string | null>(null);
  const [recapsResults, setRecapsResults] = React.useState<BrokerFanoutResult[] | null>(null);

  const [salienceBusy, setSalienceBusy] = React.useState<boolean>(false);
  const [salienceError, setSalienceError] = React.useState<string | null>(null);
  const [salienceResults, setSalienceResults] = React.useState<BrokerFanoutResult[] | null>(null);

  React.useEffect(() => {
    if (!otaStatusCacheKey) {
      setOtaStatusResults(null);
      setOtaStatusCachedAt(null);
      return;
    }
    if (typeof window === "undefined") return;
    setOtaStatusResults(null);
    setOtaStatusCachedAt(null);
    try {
      const raw = window.localStorage.getItem(otaStatusCacheKey);
      if (!raw) return;
      const parsed = JSON.parse(raw);
      const ts = typeof parsed?.ts === "number" ? parsed.ts : 0;
      if (ts <= 0) return;
      if (Date.now() - ts > OTA_STATUS_CACHE_TTL_MS) {
        window.localStorage.removeItem(otaStatusCacheKey);
        return;
      }
      const rows = normalizeBrokerFanoutResults(parsed?.results);
      if (!rows) return;
      setOtaStatusResults(rows);
      setOtaStatusCachedAt(fmtTs(ts));
    } catch {
      // ignore cache errors
    }
  }, [otaStatusCacheKey]);

  const persistOtaStatusCache = React.useCallback(
    (rows: BrokerFanoutResult[]) => {
      if (!otaStatusCacheKey || typeof window === "undefined") return;
      try {
        const ts = Date.now();
        window.localStorage.setItem(otaStatusCacheKey, JSON.stringify({ ts, results: rows }));
        setOtaStatusCachedAt(fmtTs(ts));
      } catch {
        // ignore cache errors
      }
    },
    [otaStatusCacheKey],
  );

  const clearOtaStatusCache = React.useCallback(() => {
    if (!otaStatusCacheKey || typeof window === "undefined") return;
    try {
      window.localStorage.removeItem(otaStatusCacheKey);
    } catch {
      // ignore cache errors
    }
    setOtaStatusResults(null);
    setOtaStatusCachedAt(null);
  }, [otaStatusCacheKey]);

  React.useEffect(() => {
    if (!agentId) {
      setSelectedDeployments([]);
      return;
    }
    const deployments = deploymentsQuery.data?.deployments ?? [];
    if (deployments.length === 0) {
      setSelectedDeployments([]);
      return;
    }
    const connected = deployments.filter((d) => d?.connected === true).map((d) => normalizeDeploymentId(d?.deployment_id));
    const all = deployments.map((d) => normalizeDeploymentId(d?.deployment_id));
    setSelectedDeployments(connected.length > 0 ? connected : all);
  }, [agentId, deploymentsQuery.data]);

  const upsertMutation = useMutation({
    mutationFn: async (req: { userSub: string; role: string }) => {
      const res = await apiBrokerUpsertMember(base, agentId, { user_sub: req.userSub, role: req.role }, args.auth);
      if (!res.ok) throw new Error(res.error || "upsert failed");
      return res;
    },
    onSuccess: () => {
      void membersQuery.refetch();
      void auditQuery.refetch();
    },
  });

  const deleteMutation = useMutation({
    mutationFn: async (userSub: string) => {
      const res = await apiBrokerDeleteMember(base, agentId, userSub, args.auth);
      if (!res.ok) throw new Error(res.error || "delete failed");
      return res;
    },
    onSuccess: () => {
      void membersQuery.refetch();
      void auditQuery.refetch();
    },
  });

  const [newUserSub, setNewUserSub] = React.useState<string>("");
  const [newRole, setNewRole] = React.useState<string>("user");
  const [actionError, setActionError] = React.useState<string | null>(null);

  const onUpsert = React.useCallback(async () => {
    setActionError(null);
    const userSub = String(newUserSub || "").trim();
    if (!userSub) {
      setActionError("missing user_sub");
      return;
    }
    const role = String(newRole || "user").trim().toLowerCase();
    if (role !== "user" && role !== "admin" && role !== "owner") {
      setActionError("invalid role");
      return;
    }
    try {
      await upsertMutation.mutateAsync({ userSub, role });
      setNewUserSub("");
    } catch (e) {
      setActionError(String(e));
    }
  }, [newRole, newUserSub, upsertMutation]);

  const onDelete = React.useCallback(
    async (userSub: string) => {
      setActionError(null);
      if (!window.confirm(`Remove ${userSub}?`)) return;
      try {
        await deleteMutation.mutateAsync(userSub);
      } catch (e) {
        setActionError(String(e));
      }
    },
    [deleteMutation],
  );

  const toggleDeployment = React.useCallback((id: string) => {
    setSelectedDeployments((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return Array.from(next);
    });
  }, []);

  const selectAllDeployments = React.useCallback(
    (deployments: BrokerDeploymentInfo[]) => {
      setSelectedDeployments(deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
    },
    [],
  );

  const selectConnectedDeployments = React.useCallback((deployments: BrokerDeploymentInfo[]) => {
    const connected = deployments.filter((d) => d?.connected === true).map((d) => normalizeDeploymentId(d?.deployment_id));
    setSelectedDeployments(connected.length > 0 ? connected : deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
  }, []);

  const parseOptionalInt = React.useCallback((raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseInt(s, 10);
    if (!Number.isFinite(n) || n < min) return undefined;
    return n;
  }, []);

  const parseOptionalFloat = React.useCallback((raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseFloat(s);
    if (!Number.isFinite(n) || n < min) return undefined;
    return n;
  }, []);

  const runOtaUpdate = React.useCallback(async () => {
    setOtaError(null);
    setOtaResults(null);
    setOtaStatusResults(null);
    clearOtaStatusCache();
    const url = String(otaUrl || "").trim();
    if (!url) return void setOtaError("missing OTA url");
    if (!agentId) return void setOtaError("missing agent_id");
    if (selectedDeployments.length === 0) return void setOtaError("select at least one deployment");
    const drainMs = Number.parseInt(String(otaDrainMs || ""), 10);
    const drainTimeout = Number.isFinite(drainMs) && drainMs >= 0 ? drainMs : undefined;
    const body: Record<string, unknown> = { url };
    const sha = String(otaSha256 || "").trim();
    if (sha) body.sha256 = sha;
    const ver = String(otaVersion || "").trim();
    if (ver) body.version = ver;
    const reason = String(otaReason || "").trim();
    if (reason) body.reason = reason;
    if (drainTimeout !== undefined) body.drain_timeout_ms = drainTimeout;
    setOtaBusy(true);
    try {
      const bulk = await apiBrokerOtaUpdateBulk(base, agentId, body, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerOtaUpdate(base, agentId, body, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setOtaResults(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setOtaResults(results);
    } catch (e) {
      setOtaError(String(e));
    } finally {
      setOtaBusy(false);
    }
  }, [
    agentId,
    args.auth,
    base,
    clearOtaStatusCache,
    otaDrainMs,
    otaReason,
    otaSha256,
    otaUrl,
    otaVersion,
    selectedDeployments,
  ]);

  const runOtaStatus = React.useCallback(async () => {
    setOtaStatusError(null);
    setOtaStatusResults(null);
    if (!agentId) return void setOtaStatusError("missing agent_id");
    if (selectedDeployments.length === 0) return void setOtaStatusError("select at least one deployment");
    setOtaStatusBusy(true);
    try {
      const bulk = await apiBrokerOtaStatusBulk(base, agentId, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerOtaStatus(base, agentId, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setOtaStatusResults(results);
        persistOtaStatusCache(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setOtaStatusResults(results);
      persistOtaStatusCache(results);
    } catch (e) {
      setOtaStatusError(String(e));
    } finally {
      setOtaStatusBusy(false);
    }
  }, [agentId, args.auth, base, persistOtaStatusCache, selectedDeployments]);

  const runRetention = React.useCallback(async () => {
    setRetentionError(null);
    setRetentionResults(null);
    if (!agentId) return void setRetentionError("missing agent_id");
    if (selectedDeployments.length === 0) return void setRetentionError("select at least one deployment");
    const payload: Record<string, unknown> = { dry_run: retentionDryRun };
    const dailyDays = parseOptionalInt(retentionDailyMaxDays, 0);
    if (dailyDays !== undefined) payload.daily_max_days = dailyDays;
    const dailyBytes = parseOptionalInt(retentionDailyMaxBytes, 0);
    if (dailyBytes !== undefined) payload.daily_max_bytes = dailyBytes;
    const checkpointDays = parseOptionalInt(retentionCheckpointMaxDays, 0);
    if (checkpointDays !== undefined) payload.checkpoint_max_days = checkpointDays;
    const checkpointCount = parseOptionalInt(retentionCheckpointMaxCount, 0);
    if (checkpointCount !== undefined) payload.checkpoint_max_count = checkpointCount;
    const deprecateDays = parseOptionalInt(retentionStructuredDeprecateDays, 0);
    if (deprecateDays !== undefined) payload.structured_deprecate_days = deprecateDays;
    const deprecateMax = parseOptionalInt(retentionStructuredDeprecateMaxEntries, 0);
    if (deprecateMax !== undefined) payload.structured_deprecate_max_entries = deprecateMax;
    setRetentionBusy(true);
    try {
      const bulk = await apiBrokerMemoryRetentionBulk(base, agentId, payload, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, "/api/v1/memory/retention/enforce", "POST", payload, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setRetentionResults(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRetentionResults(results);
    } catch (e) {
      setRetentionError(String(e));
    } finally {
      setRetentionBusy(false);
    }
  }, [
    agentId,
    args.auth,
    base,
    parseOptionalInt,
    retentionCheckpointMaxCount,
    retentionCheckpointMaxDays,
    retentionDailyMaxBytes,
    retentionDailyMaxDays,
    retentionDryRun,
    retentionStructuredDeprecateDays,
    retentionStructuredDeprecateMaxEntries,
    selectedDeployments,
  ]);

  const runRecapsList = React.useCallback(async () => {
    setRecapsError(null);
    setRecapsResults(null);
    if (!agentId) return void setRecapsError("missing agent_id");
    if (selectedDeployments.length === 0) return void setRecapsError("select at least one deployment");
    const listParams = {
      limit: parseOptionalInt(recapsLimit, 1) ?? 20,
      includeSummary: recapsIncludeSummary,
      kind: recapsKindFilter.trim() || undefined,
    };
    setRecapsListBusy(true);
    try {
      const bulk = await apiBrokerMemoryRecapsListBulk(base, agentId, listParams, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const qs = new URLSearchParams();
        qs.set("limit", String(listParams.limit));
        if (listParams.includeSummary) qs.set("include_summary", "1");
        if (listParams.kind) qs.set("kind", listParams.kind);
        const path = `/api/v1/memory/recaps${qs.toString() ? `?${qs.toString()}` : ""}`;
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, path, "GET", undefined, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setRecapsResults(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRecapsResults(results);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsListBusy(false);
    }
  }, [agentId, args.auth, base, parseOptionalInt, recapsIncludeSummary, recapsKindFilter, recapsLimit, selectedDeployments]);

  const runRecapsGenerate = React.useCallback(async () => {
    setRecapsError(null);
    setRecapsResults(null);
    if (!agentId) return void setRecapsError("missing agent_id");
    if (selectedDeployments.length === 0) return void setRecapsError("select at least one deployment");
    const payload: Record<string, unknown> = {
      dry_run: recapsDryRun,
      write_file: recapsWriteFile,
      include_structured: recapsIncludeStructured,
      include_daily: recapsIncludeDaily,
    };
    const kind = recapsKind.trim();
    if (kind) payload.kind = kind;
    const model = recapsModel.trim();
    if (model) payload.model = model;
    const summaryMax = parseOptionalInt(recapsSummaryMaxChars, 0);
    if (summaryMax !== undefined) payload.summary_max_chars = summaryMax;
    const dailyDays = parseOptionalInt(recapsDailyDays, 0);
    if (dailyDays !== undefined) payload.daily_days = dailyDays;
    const maxItems = parseOptionalInt(recapsMaxItems, 0);
    if (maxItems !== undefined) payload.max_items = maxItems;
    const maxStructured = parseOptionalInt(recapsStructuredMaxItems, 0);
    if (maxStructured !== undefined) payload.max_structured_items = maxStructured;
    const maxDaily = parseOptionalInt(recapsDailyMaxItems, 0);
    if (maxDaily !== undefined) payload.max_daily_items = maxDaily;
    const halfLife = parseOptionalFloat(recapsHalfLifeDays, 0);
    if (halfLife !== undefined) payload.half_life_days = halfLife;
    const importance = parseOptionalFloat(recapsImportanceWeight, 0);
    if (importance !== undefined) payload.importance_weight = importance;
    setRecapsGenerateBusy(true);
    try {
      const bulk = await apiBrokerMemoryRecapsCreateBulk(base, agentId, payload, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, "/api/v1/memory/recaps", "POST", payload, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setRecapsResults(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRecapsResults(results);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsGenerateBusy(false);
    }
  }, [
    agentId,
    args.auth,
    base,
    parseOptionalFloat,
    parseOptionalInt,
    recapsDailyDays,
    recapsDailyMaxItems,
    recapsDryRun,
    recapsHalfLifeDays,
    recapsImportanceWeight,
    recapsIncludeDaily,
    recapsIncludeStructured,
    recapsKind,
    recapsMaxItems,
    recapsModel,
    recapsStructuredMaxItems,
    recapsSummaryMaxChars,
    recapsWriteFile,
    selectedDeployments,
  ]);

  const runSalience = React.useCallback(async () => {
    setSalienceError(null);
    setSalienceResults(null);
    if (!agentId) return void setSalienceError("missing agent_id");
    if (selectedDeployments.length === 0) return void setSalienceError("select at least one deployment");
    const params = {
      includeStructured: recapsIncludeStructured,
      includeDaily: recapsIncludeDaily,
      dailyDays: parseOptionalInt(recapsDailyDays, 0),
      maxItems: parseOptionalInt(recapsMaxItems, 0),
      maxStructuredItems: parseOptionalInt(recapsStructuredMaxItems, 0),
      maxDailyItems: parseOptionalInt(recapsDailyMaxItems, 0),
      halfLifeDays: parseOptionalFloat(recapsHalfLifeDays, 0),
      importanceWeight: parseOptionalFloat(recapsImportanceWeight, 0),
    };
    setSalienceBusy(true);
    try {
      const bulk = await apiBrokerMemorySalienceBulk(base, agentId, params, args.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const qs = new URLSearchParams();
        qs.set("include_structured", params.includeStructured ? "1" : "0");
        qs.set("include_daily", params.includeDaily ? "1" : "0");
        if (params.dailyDays !== undefined) qs.set("daily_days", String(params.dailyDays));
        if (params.maxItems !== undefined) qs.set("max_items", String(params.maxItems));
        if (params.maxStructuredItems !== undefined) qs.set("max_structured_items", String(params.maxStructuredItems));
        if (params.maxDailyItems !== undefined) qs.set("max_daily_items", String(params.maxDailyItems));
        if (params.halfLifeDays !== undefined) qs.set("half_life_days", String(params.halfLifeDays));
        if (params.importanceWeight !== undefined) qs.set("importance_weight", String(params.importanceWeight));
        const path = `/api/v1/memory/salience${qs.toString() ? `?${qs.toString()}` : ""}`;
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, path, "GET", undefined, args.auth, deploymentId);
            return { deployment_id: deploymentId, status: res.status, data: res.data };
          }),
        );
        const results = settled.map((r, idx) =>
          r.status === "fulfilled"
            ? r.value
            : { deployment_id: selectedDeployments[idx], status: 0, data: { ok: false, error: String(r.reason || "request failed") } },
        );
        setSalienceResults(results);
        return;
      }
      if (bulk.status >= 400) throw new Error(bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`);
      const results = normalizeBrokerFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setSalienceResults(results);
    } catch (e) {
      setSalienceError(String(e));
    } finally {
      setSalienceBusy(false);
    }
  }, [
    agentId,
    args.auth,
    base,
    parseOptionalFloat,
    parseOptionalInt,
    recapsDailyDays,
    recapsDailyMaxItems,
    recapsHalfLifeDays,
    recapsImportanceWeight,
    recapsIncludeDaily,
    recapsIncludeStructured,
    recapsMaxItems,
    recapsStructuredMaxItems,
    selectedDeployments,
  ]);

  React.useEffect(() => {
    if (!args.open || !canQuery || !agentId) return;
    if (otaStatusBusy || selectedDeployments.length === 0) return;
    if (otaStatusResults || otaStatusCachedAt) return;
    void runOtaStatus();
  }, [agentId, args.open, canQuery, otaStatusBusy, otaStatusCachedAt, otaStatusResults, runOtaStatus, selectedDeployments]);

  const agents: BrokerAgentInfo[] = agentsQuery.data?.agents ?? [];
  const connectors = connectorsQuery.data?.connectors ?? [];
  const members = membersQuery.data?.members ?? [];
  const ownerSub = String(membersQuery.data?.owner_sub || "");
  const auditRows = auditQuery.data?.audit ?? [];
  const deployments: BrokerDeploymentInfo[] = deploymentsQuery.data?.deployments ?? [];
  const defaultDeploymentIdRaw = deploymentsQuery.data?.default_deployment_id;
  const defaultDeploymentId = defaultDeploymentIdRaw ? normalizeDeploymentId(defaultDeploymentIdRaw) : "";
  const selectedDeploymentSet = new Set(selectedDeployments);

  return {
    actionError,
    agents,
    agentsQuery,
    agentId,
    args,
    auditLimit,
    auditQuery,
    auditRows,
    base,
    brokerEventsState,
    brokerPage,
    brokerPages,
    canQuery,
    clearOtaStatusCache,
    connectorStaleMinutes,
    connectorStaleMs,
    connectors,
    connectorsQuery,
    defaultDeploymentId,
    deleteMutation,
    deployments,
    deploymentsQuery,
    effectiveStaleMinutes,
    limitValue,
    members,
    membersQuery,
    newRole,
    newUserSub,
    onDelete,
    onUpsert,
    otaBusy,
    otaDrainMs,
    otaError,
    otaReason,
    otaResults,
    otaSha256,
    otaStatusBusy,
    otaStatusCachedAt,
    otaStatusError,
    otaStatusResults,
    otaUrl,
    otaVersion,
    ownerSub,
    recapsDailyDays,
    recapsDailyMaxItems,
    recapsDryRun,
    recapsError,
    recapsGenerateBusy,
    recapsHalfLifeDays,
    recapsImportanceWeight,
    recapsIncludeDaily,
    recapsIncludeStructured,
    recapsIncludeSummary,
    recapsKind,
    recapsKindFilter,
    recapsLimit,
    recapsListBusy,
    recapsMaxItems,
    recapsModel,
    recapsResults,
    recapsStructuredMaxItems,
    recapsSummaryMaxChars,
    recapsWriteFile,
    retentionBusy,
    retentionCheckpointMaxCount,
    retentionCheckpointMaxDays,
    retentionDailyMaxBytes,
    retentionDailyMaxDays,
    retentionDryRun,
    retentionError,
    retentionResults,
    retentionStructuredDeprecateDays,
    retentionStructuredDeprecateMaxEntries,
    runOtaStatus,
    runOtaUpdate,
    runRecapsGenerate,
    runRecapsList,
    runRetention,
    runSalience,
    salienceBusy,
    salienceError,
    salienceResults,
    selectAllDeployments,
    selectConnectedDeployments,
    selectedDeploymentSet,
    selectedDeployments,
    setAuditLimit,
    setBrokerPage,
    setConnectorStaleMinutes,
    setNewRole,
    setNewUserSub,
    setOtaDrainMs,
    setOtaError,
    setOtaReason,
    setOtaResults,
    setOtaSha256,
    setOtaStatusError,
    setOtaStatusResults,
    setOtaUrl,
    setOtaVersion,
    setRecapsDailyDays,
    setRecapsDailyMaxItems,
    setRecapsDryRun,
    setRecapsError,
    setRecapsHalfLifeDays,
    setRecapsImportanceWeight,
    setRecapsIncludeDaily,
    setRecapsIncludeStructured,
    setRecapsIncludeSummary,
    setRecapsKind,
    setRecapsKindFilter,
    setRecapsLimit,
    setRecapsMaxItems,
    setRecapsModel,
    setRecapsResults,
    setRecapsStructuredMaxItems,
    setRecapsSummaryMaxChars,
    setRecapsWriteFile,
    setRetentionCheckpointMaxCount,
    setRetentionCheckpointMaxDays,
    setRetentionDailyMaxBytes,
    setRetentionDailyMaxDays,
    setRetentionDryRun,
    setRetentionError,
    setRetentionResults,
    setRetentionStructuredDeprecateDays,
    setRetentionStructuredDeprecateMaxEntries,
    setSalienceError,
    setSalienceResults,
    setSelectedDeployments,
    toggleDeployment,
    upsertMutation,
    useCookieAuth,
    exportConnectors: () => apiBrokerExportConnectors(base, args.auth),
  };
}
