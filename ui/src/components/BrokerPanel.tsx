import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiBrokerDeleteMember,
  apiBrokerGetMembers,
  apiBrokerGetMembershipAudit,
  apiBrokerListAgents,
  apiBrokerListDeployments,
  apiBrokerMemoryRecapsCreateBulk,
  apiBrokerMemoryRecapsListBulk,
  apiBrokerMemorySalienceBulk,
  apiBrokerMemoryRetentionBulk,
  apiBrokerOtaStatus,
  apiBrokerOtaStatusBulk,
  apiBrokerOtaUpdate,
  apiBrokerOtaUpdateBulk,
  apiBrokerProxyJson,
  apiBrokerUpsertMember,
  daemonHeaders,
  type ApiAuth,
} from "../api";
import FieldLabel from "./FieldLabel";
import BrokerTeamConsole from "./broker/BrokerTeamConsole";
import type { BrokerEventRow } from "./broker/types";
import { readSseStream } from "../sse";

const normalizeBrokerBase = (raw: string) => {
  const base = String(raw || "").trim();
  if (!base) return "";
  const withScheme = /^https?:\/\//i.test(base) ? base : `https://${base}`;
  return withScheme.replace(/\/+$/, "");
};

const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

const BROKER_EVENTS_MAX = 200;

export type BrokerPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  brokerBase: string;
  brokerAgentId: string;
  setBrokerAgentId: (next: string) => void;
  auth: ApiAuth;
  authKey: string;
};

export default function BrokerPanel(props: BrokerPanelProps) {
  const base = React.useMemo(() => normalizeBrokerBase(props.brokerBase), [props.brokerBase]);
  const agentId = String(props.brokerAgentId || "").trim();
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const canQuery = base.length > 0 && authToken.length > 0;

  const agentsQuery = useQuery({
    queryKey: ["brokerAgents", base, props.authKey],
    enabled: false,
    queryFn: () => apiBrokerListAgents(base, props.auth),
  });

  const membersQuery = useQuery({
    queryKey: ["brokerMembers", base, props.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerGetMembers(base, agentId, props.auth),
  });

  const deploymentsQuery = useQuery({
    queryKey: ["brokerDeployments", base, props.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerListDeployments(base, agentId, props.auth),
  });

  const normalizeDeploymentId = (raw: unknown) => {
    const id = String(raw || "").trim();
    return id || "default";
  };

  const [selectedDeployments, setSelectedDeployments] = React.useState<string[]>([]);
  const [otaUrl, setOtaUrl] = React.useState<string>("");
  const [otaSha256, setOtaSha256] = React.useState<string>("");
  const [otaVersion, setOtaVersion] = React.useState<string>("");
  const [otaDrainMs, setOtaDrainMs] = React.useState<string>("15000");
  const [otaReason, setOtaReason] = React.useState<string>("");
  const [otaBusy, setOtaBusy] = React.useState<boolean>(false);
  const [otaError, setOtaError] = React.useState<string | null>(null);
  const [otaResults, setOtaResults] = React.useState<any[] | null>(null);
  const [otaStatusBusy, setOtaStatusBusy] = React.useState<boolean>(false);
  const [otaStatusError, setOtaStatusError] = React.useState<string | null>(null);
  const [otaStatusResults, setOtaStatusResults] = React.useState<any[] | null>(null);
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
  const [retentionResults, setRetentionResults] = React.useState<any[] | null>(null);

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
  const [recapsResults, setRecapsResults] = React.useState<any[] | null>(null);

  const [salienceBusy, setSalienceBusy] = React.useState<boolean>(false);
  const [salienceError, setSalienceError] = React.useState<string | null>(null);
  const [salienceResults, setSalienceResults] = React.useState<any[] | null>(null);

  const [auditLimit, setAuditLimit] = React.useState<string>("200");
  const [brokerEvents, setBrokerEvents] = React.useState<BrokerEventRow[]>([]);
  const [brokerEventsError, setBrokerEventsError] = React.useState<string | null>(null);
  const [brokerEventsActive, setBrokerEventsActive] = React.useState<boolean>(true);
  const [brokerEventsConnected, setBrokerEventsConnected] = React.useState<boolean>(false);
  const [brokerEventsQuorumOnly, setBrokerEventsQuorumOnly] = React.useState<boolean>(true);
  const limitValue = React.useMemo(() => {
    const n = Number.parseInt(String(auditLimit || ""), 10);
    if (!Number.isFinite(n) || n <= 0) return 200;
    return Math.min(Math.max(n, 1), 500);
  }, [auditLimit]);

  const auditQuery = useQuery({
    queryKey: ["brokerMembershipAudit", base, props.authKey, agentId, limitValue],
    enabled: false,
    queryFn: () => apiBrokerGetMembershipAudit(base, agentId, limitValue, props.auth),
  });

  React.useEffect(() => {
    if (!props.open || !canQuery) return;
    if (!agentsQuery.data && !agentsQuery.isFetching) {
      void agentsQuery.refetch();
    }
  }, [props.open, canQuery, agentsQuery]);

  React.useEffect(() => {
    if (!props.open || !canQuery || !agentId) return;
    if (!membersQuery.data && !membersQuery.isFetching) {
      void membersQuery.refetch();
    }
    if (!auditQuery.data && !auditQuery.isFetching) {
      void auditQuery.refetch();
    }
    if (!deploymentsQuery.data && !deploymentsQuery.isFetching) {
      void deploymentsQuery.refetch();
    }
  }, [props.open, canQuery, agentId, membersQuery, auditQuery, deploymentsQuery]);

  React.useEffect(() => {
    if (!props.open || !canQuery || !brokerEventsActive) {
      setBrokerEventsConnected(false);
      return;
    }
    const controller = new AbortController();
    const run = async () => {
      setBrokerEventsError(null);
      setBrokerEventsConnected(false);
      try {
        const resp = await fetch(`${base}/v1/events`, {
          headers: daemonHeaders(props.auth),
          signal: controller.signal,
        });
        if (!resp.ok) {
          throw new Error(`broker events failed (${resp.status})`);
        }
        setBrokerEventsConnected(true);
        await readSseStream(resp, (ev) => {
          if (controller.signal.aborted) return;
          let parsed: any = null;
          try {
            parsed = ev.data ? JSON.parse(ev.data) : null;
          } catch {
            parsed = null;
          }
          const eventType = String(parsed?.type || ev.event || "message");
          const row: BrokerEventRow = {
            type: eventType,
            ts_unix_ms: Number(parsed?.ts_unix_ms || 0) || undefined,
            event_id: parsed?.event_id ? String(parsed.event_id) : undefined,
            trace_id: parsed?.trace_id ? String(parsed.trace_id) : undefined,
            payload: parsed?.payload && typeof parsed.payload === "object" ? parsed.payload : undefined,
          };
          setBrokerEvents((prev) => {
            const next = prev.concat(row);
            if (next.length <= BROKER_EVENTS_MAX) return next;
            return next.slice(next.length - BROKER_EVENTS_MAX);
          });
        });
      } catch (err) {
        if (controller.signal.aborted) return;
        setBrokerEventsConnected(false);
        setBrokerEventsError(String(err));
      }
    };
    void run();
    return () => controller.abort();
  }, [props.open, canQuery, base, props.authKey, brokerEventsActive]);

  const otaStatusCacheKey = React.useMemo(() => {
    if (!base || !agentId) return "";
    return `agentd:broker:otaStatus:${base}:${agentId}`;
  }, [base, agentId]);
  const otaStatusCacheTtlMs = 10 * 60 * 1000;

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
      if (Date.now() - ts > otaStatusCacheTtlMs) {
        window.localStorage.removeItem(otaStatusCacheKey);
        setOtaStatusResults(null);
        setOtaStatusCachedAt(null);
        return;
      }
      const rows = Array.isArray(parsed?.results) ? parsed.results : null;
      if (!rows) return;
      setOtaStatusResults(rows);
      setOtaStatusCachedAt(fmtTs(ts));
    } catch {
      // ignore cache errors
    }
  }, [otaStatusCacheKey, otaStatusCacheTtlMs]);

  const persistOtaStatusCache = (rows: any[]) => {
    if (!otaStatusCacheKey) return;
    if (typeof window === "undefined") return;
    try {
      const ts = Date.now();
      window.localStorage.setItem(otaStatusCacheKey, JSON.stringify({ ts, results: rows }));
      setOtaStatusCachedAt(fmtTs(ts));
    } catch {
      // ignore cache errors
    }
  };

  const clearOtaStatusCache = () => {
    if (!otaStatusCacheKey) return;
    if (typeof window === "undefined") return;
    try {
      window.localStorage.removeItem(otaStatusCacheKey);
    } catch {
      // ignore cache errors
    }
    setOtaStatusResults(null);
    setOtaStatusCachedAt(null);
  };

  React.useEffect(() => {
    if (!agentId) {
      setSelectedDeployments([]);
      return;
    }
    const deployments = Array.isArray((deploymentsQuery.data as any)?.deployments)
      ? (((deploymentsQuery.data as any).deployments as any[]) ?? [])
      : [];
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
      const res = await apiBrokerUpsertMember(base, agentId, { user_sub: req.userSub, role: req.role }, props.auth);
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
      const res = await apiBrokerDeleteMember(base, agentId, userSub, props.auth);
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

  const onUpsert = async () => {
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
  };

  const onDelete = async (userSub: string) => {
    setActionError(null);
    if (!window.confirm(`Remove ${userSub}?`)) return;
    try {
      await deleteMutation.mutateAsync(userSub);
    } catch (e) {
      setActionError(String(e));
    }
  };

  const toggleDeployment = (id: string) => {
    setSelectedDeployments((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return Array.from(next);
    });
  };

  const selectAllDeployments = () => {
    setSelectedDeployments(deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
  };

  const selectConnectedDeployments = () => {
    const connected = deployments.filter((d) => d?.connected === true).map((d) => normalizeDeploymentId(d?.deployment_id));
    setSelectedDeployments(connected.length > 0 ? connected : deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
  };

  const normalizeFanoutResults = (rows: any[], fallbackIds?: string[]) => {
    if (!Array.isArray(rows)) return null;
    return rows.map((row, idx) => {
      const rec = row ?? {};
      const dep =
        normalizeDeploymentId(rec?.deployment_id ?? rec?.deploymentId ?? rec?.deployment ?? fallbackIds?.[idx] ?? "");
      const statusRaw = rec?.status;
      const status = typeof statusRaw === "number" ? statusRaw : Number(statusRaw || 0);
      const data = rec?.data ?? rec?.response ?? rec?.body ?? rec;
      return { deployment_id: dep, status, data };
    });
  };

  const parseOptionalInt = (raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseInt(s, 10);
    if (!Number.isFinite(n) || n < min) return undefined;
    return n;
  };

  const parseOptionalFloat = (raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseFloat(s);
    if (!Number.isFinite(n) || n < min) return undefined;
    return n;
  };

  const runOtaUpdate = async () => {
    setOtaError(null);
    setOtaResults(null);
    setOtaStatusResults(null);
    clearOtaStatusCache();
    const url = String(otaUrl || "").trim();
    if (!url) {
      setOtaError("missing OTA url");
      return;
    }
    if (!agentId) {
      setOtaError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setOtaError("select at least one deployment");
      return;
    }
    const drainMs = Number.parseInt(String(otaDrainMs || ""), 10);
    const drainTimeout = Number.isFinite(drainMs) && drainMs >= 0 ? drainMs : undefined;
    const body: Record<string, any> = {
      url,
    };
    const sha = String(otaSha256 || "").trim();
    if (sha) body.sha256 = sha;
    const ver = String(otaVersion || "").trim();
    if (ver) body.version = ver;
    const reason = String(otaReason || "").trim();
    if (reason) body.reason = reason;
    if (drainTimeout !== undefined) body.drain_timeout_ms = drainTimeout;

    setOtaBusy(true);
    try {
      const bulk = await apiBrokerOtaUpdateBulk(base, agentId, body, props.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerOtaUpdate(base, agentId, body, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setOtaResults(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) {
        throw new Error("unexpected broker response");
      }
      setOtaResults(results);
    } catch (e) {
      setOtaError(String(e));
    } finally {
      setOtaBusy(false);
    }
  };

  const runOtaStatus = async () => {
    setOtaStatusError(null);
    setOtaStatusResults(null);
    if (!agentId) {
      setOtaStatusError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setOtaStatusError("select at least one deployment");
      return;
    }
    setOtaStatusBusy(true);
    try {
      const bulk = await apiBrokerOtaStatusBulk(base, agentId, props.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerOtaStatus(base, agentId, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setOtaStatusResults(results);
        persistOtaStatusCache(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) {
        throw new Error("unexpected broker response");
      }
      setOtaStatusResults(results);
      persistOtaStatusCache(results);
    } catch (e) {
      setOtaStatusError(String(e));
    } finally {
      setOtaStatusBusy(false);
    }
  };

  const runRetention = async () => {
    setRetentionError(null);
    setRetentionResults(null);
    if (!agentId) {
      setRetentionError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setRetentionError("select at least one deployment");
      return;
    }
    const payload: Record<string, any> = {
      dry_run: retentionDryRun,
    };
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
      const bulk = await apiBrokerMemoryRetentionBulk(base, agentId, payload, props.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, "/api/v1/memory/retention/enforce", "POST", payload, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setRetentionResults(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRetentionResults(results);
    } catch (e) {
      setRetentionError(String(e));
    } finally {
      setRetentionBusy(false);
    }
  };

  const runRecapsList = async () => {
    setRecapsError(null);
    setRecapsResults(null);
    if (!agentId) {
      setRecapsError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setRecapsError("select at least one deployment");
      return;
    }
    const listParams = {
      limit: parseOptionalInt(recapsLimit, 1) ?? 20,
      includeSummary: recapsIncludeSummary,
      kind: recapsKindFilter.trim() || undefined,
    };

    setRecapsListBusy(true);
    try {
      const bulk = await apiBrokerMemoryRecapsListBulk(base, agentId, listParams, props.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const qs = new URLSearchParams();
        qs.set("limit", String(listParams.limit));
        if (listParams.includeSummary) qs.set("include_summary", "1");
        if (listParams.kind) qs.set("kind", listParams.kind);
        const path = `/api/v1/memory/recaps${qs.toString() ? `?${qs.toString()}` : ""}`;
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, path, "GET", undefined, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setRecapsResults(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRecapsResults(results);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsListBusy(false);
    }
  };

  const runRecapsGenerate = async () => {
    setRecapsError(null);
    setRecapsResults(null);
    if (!agentId) {
      setRecapsError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setRecapsError("select at least one deployment");
      return;
    }
    const payload: Record<string, any> = {
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
      const bulk = await apiBrokerMemoryRecapsCreateBulk(base, agentId, payload, props.auth, selectedDeployments);
      if (bulk.status === 404 || bulk.status === 405 || bulk.status === 501) {
        const settled = await Promise.allSettled(
          selectedDeployments.map(async (deploymentId) => {
            const res = await apiBrokerProxyJson(base, agentId, "/api/v1/memory/recaps", "POST", payload, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setRecapsResults(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setRecapsResults(results);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsGenerateBusy(false);
    }
  };

  const runSalience = async () => {
    setSalienceError(null);
    setSalienceResults(null);
    if (!agentId) {
      setSalienceError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setSalienceError("select at least one deployment");
      return;
    }
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
      const bulk = await apiBrokerMemorySalienceBulk(base, agentId, params, props.auth, selectedDeployments);
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
            const res = await apiBrokerProxyJson(base, agentId, path, "GET", undefined, props.auth, deploymentId);
            return {
              deployment_id: deploymentId,
              status: res.status,
              data: res.data,
            };
          }),
        );
        const results = settled.map((r, idx) => {
          if (r.status === "fulfilled") return r.value;
          return {
            deployment_id: selectedDeployments[idx],
            status: 0,
            data: { ok: false, error: String(r.reason || "request failed") },
          };
        });
        setSalienceResults(results);
        return;
      }
      if (bulk.status >= 400) {
        const err = bulk.data?.error ? String(bulk.data.error) : `broker error (${bulk.status})`;
        throw new Error(err);
      }
      const results = normalizeFanoutResults(bulk.data?.results, selectedDeployments);
      if (!results) throw new Error("unexpected broker response");
      setSalienceResults(results);
    } catch (e) {
      setSalienceError(String(e));
    } finally {
      setSalienceBusy(false);
    }
  };

  React.useEffect(() => {
    if (!props.open || !canQuery || !agentId) return;
    if (otaStatusBusy) return;
    if (selectedDeployments.length === 0) return;
    if (otaStatusResults || otaStatusCachedAt) return;
    void runOtaStatus();
  }, [props.open, canQuery, agentId, otaStatusBusy, selectedDeployments, otaStatusResults, otaStatusCachedAt]);

  const agents = Array.isArray((agentsQuery.data as any)?.agents) ? ((agentsQuery.data as any).agents as any[]) : [];
  const members = Array.isArray((membersQuery.data as any)?.members) ? ((membersQuery.data as any).members as any[]) : [];
  const ownerSub = String((membersQuery.data as any)?.owner_sub || "");
  const auditRows = Array.isArray((auditQuery.data as any)?.audit) ? ((auditQuery.data as any).audit as any[]) : [];
  const brokerEventRows = React.useMemo(() => {
    const rows = brokerEventsQuorumOnly
      ? brokerEvents.filter((ev) => String(ev?.type || "").startsWith("team_quorum"))
      : brokerEvents;
    return rows.slice().reverse();
  }, [brokerEvents, brokerEventsQuorumOnly]);
  const deployments = Array.isArray((deploymentsQuery.data as any)?.deployments)
    ? (((deploymentsQuery.data as any).deployments as any[]) ?? [])
    : [];
  const defaultDeploymentIdRaw = (deploymentsQuery.data as any)?.default_deployment_id;
  const defaultDeploymentId = defaultDeploymentIdRaw ? normalizeDeploymentId(defaultDeploymentIdRaw) : "";
  const selectedDeploymentSet = new Set(selectedDeployments);

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Broker console</div>
          <div className="text-[11px] text-white/50">Manage agents + membership when in broker mode</div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        {!base ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker base URL. Set it in Settings.
          </div>
        ) : authToken.length === 0 ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker auth token (OIDC). Set it in Settings.
          </div>
        ) : null}

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Agents</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || agentsQuery.isFetching}
              onClick={() => void agentsQuery.refetch()}
            >
              {agentsQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {agentsQuery.error ? (
            <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(agentsQuery.error)}
            </div>
          ) : null}

          {agents.length === 0 ? (
            <div className="text-[11px] text-white/50">No agents returned.</div>
          ) : (
            <div className="grid gap-2">
              {agents.map((agent) => {
                const id = String(agent?.agent_id || "");
                const connected = agent?.connected === true;
                const selected = id && id === agentId;
                return (
                  <div key={id} className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1">
                    <div className="flex flex-col">
                      <div className="text-xs text-white/90">{id}</div>
                      <div className="text-[11px] text-white/50">
                        {connected ? "connected" : "disconnected"}
                        {agent?.owner_sub ? ` · owner ${String(agent.owner_sub)}` : ""}
                      </div>
                    </div>
                    <button
                      className={
                        selected
                          ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                          : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      }
                      type="button"
                      onClick={() => props.setBrokerAgentId(id)}
                    >
                      {selected ? "Selected" : "Use"}
                    </button>
                  </div>
                );
              })}
            </div>
          )}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Members</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || membersQuery.isFetching}
              onClick={() => void membersQuery.refetch()}
            >
              {membersQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to manage membership.</div>
          ) : membersQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(membersQuery.error)}
            </div>
          ) : (
            <>
              <div className="mb-2 text-[11px] text-white/50">Owner: {ownerSub || "(unknown)"}</div>
              <div className="grid gap-2">
                {members.length === 0 ? (
                  <div className="text-[11px] text-white/50">No members.</div>
                ) : (
                  members.map((member) => {
                    const userSub = String(member?.user_sub || "");
                    const role = String(member?.role || "user");
                    const created = fmtTs(member?.created_unix_ms);
                    const isOwner = role === "owner" || userSub === ownerSub;
                    return (
                      <div
                        key={`${userSub}-${role}`}
                        className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                      >
                        <div className="flex flex-col">
                          <div className="text-xs text-white/90">{userSub}</div>
                          <div className="text-[11px] text-white/50">
                            role: {role}
                            {created ? ` · added ${created}` : ""}
                          </div>
                        </div>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                          type="button"
                          disabled={isOwner || deleteMutation.isPending}
                          title={isOwner ? "Owner cannot be removed." : "Remove member"}
                          onClick={() => onDelete(userSub)}
                        >
                          Remove
                        </button>
                      </div>
                    );
                  })
                )}
              </div>

              <div className="mt-3 grid gap-2">
                <FieldLabel>Add / update member</FieldLabel>
                <div className="flex flex-wrap items-center gap-2">
                  <input
                    className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                    placeholder="user_sub"
                    value={newUserSub}
                    onChange={(e) => setNewUserSub(e.target.value)}
                  />
                  <select
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                    value={newRole}
                    onChange={(e) => setNewRole(e.target.value)}
                  >
                    <option value="user">user</option>
                    <option value="admin">admin</option>
                  </select>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={!canQuery || !agentId || upsertMutation.isPending}
                    onClick={() => void onUpsert()}
                  >
                    {upsertMutation.isPending ? "Saving…" : "Save"}
                  </button>
                </div>
                {actionError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                    {actionError}
                  </div>
                ) : null}
              </div>
            </>
          )}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Deployments + OTA</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || deploymentsQuery.isFetching}
              onClick={() => void deploymentsQuery.refetch()}
            >
              {deploymentsQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to manage deployments.</div>
          ) : deploymentsQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(deploymentsQuery.error)}
            </div>
          ) : deployments.length === 0 ? (
            <div className="text-[11px] text-white/50">No deployments connected.</div>
          ) : (
            <>
              {defaultDeploymentId ? (
                <div className="mb-2 text-[11px] text-white/50">
                  Broker default: <span className="font-mono text-white/80">{defaultDeploymentId}</span>
                </div>
              ) : null}
              <div className="grid gap-2">
                {deployments.map((dep) => {
                  const id = normalizeDeploymentId(dep?.deployment_id);
                  const selected = selectedDeploymentSet.has(id);
                  const connected = dep?.connected === true;
                  return (
                    <label
                      key={id}
                      className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                    >
                      <div className="flex items-center gap-2">
                        <input
                          type="checkbox"
                          checked={selected}
                          onChange={() => toggleDeployment(id)}
                        />
                        <div className="flex flex-col">
                          <div className="text-xs text-white/90">{id}</div>
                          <div className="text-[11px] text-white/50">
                            {connected ? "connected" : "disconnected"}
                          </div>
                        </div>
                      </div>
                      <button
                        className={
                          selected
                            ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                            : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        }
                        type="button"
                        onClick={() => toggleDeployment(id)}
                      >
                        {selected ? "Selected" : "Select"}
                      </button>
                    </label>
                  );
                })}
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={selectConnectedDeployments}
                >
                  Select connected
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={selectAllDeployments}
                >
                  Select all
                </button>
              </div>
            </>
          )}

          <div className="mt-3 grid gap-2">
            <FieldLabel>OTA update</FieldLabel>
            <input
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
              placeholder="https://.../agentd.tar.gz"
              value={otaUrl}
              onChange={(e) => setOtaUrl(e.target.value)}
            />
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="sha256 (optional)"
                value={otaSha256}
                onChange={(e) => setOtaSha256(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="version label (optional)"
                value={otaVersion}
                onChange={(e) => setOtaVersion(e.target.value)}
              />
            </div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="drain timeout ms (default 15000)"
                value={otaDrainMs}
                onChange={(e) => setOtaDrainMs(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="reason (optional)"
                value={otaReason}
                onChange={(e) => setOtaReason(e.target.value)}
              />
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!agentId || otaBusy || selectedDeployments.length === 0}
              onClick={() => void runOtaUpdate()}
            >
              {otaBusy ? "Updating…" : "Run OTA update"}
            </button>
            {otaError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {otaError}
              </div>
            ) : null}
            {otaResults && otaResults.length > 0 ? (
              <div className="grid gap-2">
                {otaResults.map((row) => {
                  const depId = String(row?.deployment_id || "");
                  const status = row?.status;
                  const ok = row?.data?.ok === true;
                  const err = row?.data?.error || row?.data?.err;
                  const respStatus = row?.data?.status || "";
                  return (
                    <div
                      key={`ota-${depId}`}
                      className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-xs text-white/90">
                        {depId} · {ok ? "ok" : "error"} · http {status}
                      </div>
                      <div className="text-[11px] text-white/50">
                        {respStatus ? `status ${respStatus}` : "no status"}
                        {err ? ` · ${String(err)}` : ""}
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : null}

            <div className="mt-3 grid gap-2">
              <FieldLabel>OTA status</FieldLabel>
              {otaStatusCachedAt ? (
                <div className="flex items-center justify-between text-[11px] text-white/50">
                  <span>Cached: {otaStatusCachedAt} (ttl 10m)</span>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/60 hover:bg-black/40"
                    type="button"
                    onClick={() => clearOtaStatusCache()}
                  >
                    Clear
                  </button>
                </div>
              ) : null}
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!agentId || otaStatusBusy || selectedDeployments.length === 0}
                onClick={() => void runOtaStatus()}
              >
                {otaStatusBusy ? "Checking…" : "Fetch OTA status"}
              </button>
              {otaStatusError ? (
                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                  {otaStatusError}
                </div>
              ) : null}
              {otaStatusResults && otaStatusResults.length > 0 ? (
                <div className="grid gap-2">
                  {otaStatusResults.map((row) => {
                    const depId = String(row?.deployment_id || "");
                    const status = row?.status;
                    const ok = row?.data?.ok === true;
                    const planStatus = row?.data?.status || "unknown";
                    const updated = fmtTs(row?.data?.updated_unix_ms);
                    const drainActive = row?.data?.drain_active === true;
                    const drainUntil = fmtTs(row?.data?.drain_until_unix_ms);
                    const drainReason = row?.data?.drain_reason;
                    const otaId = row?.data?.ota_id;
                    const jobsRunning = row?.data?.jobs_running;
                    const jobsQueued = row?.data?.jobs_queued;
                    const workflowsRunning = row?.data?.workflows_running;
                    const wfTasksRunning = row?.data?.workflow_tasks_running;
                    const wfTasksQueued = row?.data?.workflow_tasks_queued;
                    const err = row?.data?.last_error || row?.data?.error || row?.data?.err;
                    const inflightParts: string[] = [];
                    if (typeof jobsRunning === "number") inflightParts.push(`jobs ${jobsRunning}`);
                    if (typeof workflowsRunning === "number") inflightParts.push(`workflows ${workflowsRunning}`);
                    if (typeof wfTasksRunning === "number") inflightParts.push(`tasks ${wfTasksRunning}`);
                    const queuedParts: string[] = [];
                    if (typeof jobsQueued === "number") queuedParts.push(`jobs ${jobsQueued}`);
                    if (typeof wfTasksQueued === "number") queuedParts.push(`tasks ${wfTasksQueued}`);
                    return (
                      <div
                        key={`ota-status-${depId}`}
                        className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                      >
                        <div className="text-xs text-white/90">
                          {depId} · {ok ? "ok" : "error"} · http {status}
                        </div>
                        <div className="text-[11px] text-white/50">
                          {planStatus ? `status ${planStatus}` : "no status"}
                          {otaId ? ` · ota ${String(otaId)}` : ""}
                          {updated ? ` · updated ${updated}` : ""}
                          {drainActive ? " · draining" : " · idle"}
                          {drainUntil ? ` (until ${drainUntil})` : ""}
                          {drainReason ? ` · reason ${String(drainReason)}` : ""}
                          {inflightParts.length > 0 ? ` · running ${inflightParts.join(", ")}` : ""}
                          {queuedParts.length > 0 ? ` · queued ${queuedParts.join(", ")}` : ""}
                          {err ? ` · ${String(err)}` : ""}
                        </div>
                      </div>
                    );
                  })}
                </div>
              ) : null}
            </div>
          </div>
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Memory maintenance</div>
            <div className="text-[11px] text-white/50">Fan-out retention + recap operations</div>
          </div>

          <div className="grid gap-3">
            <div className="text-xs font-semibold text-white/80">Retention enforce</div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="daily max days"
                value={retentionDailyMaxDays}
                onChange={(e) => setRetentionDailyMaxDays(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="daily max bytes"
                value={retentionDailyMaxBytes}
                onChange={(e) => setRetentionDailyMaxBytes(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="checkpoint max days"
                value={retentionCheckpointMaxDays}
                onChange={(e) => setRetentionCheckpointMaxDays(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="checkpoint max count"
                value={retentionCheckpointMaxCount}
                onChange={(e) => setRetentionCheckpointMaxCount(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="structured deprecate days"
                value={retentionStructuredDeprecateDays}
                onChange={(e) => setRetentionStructuredDeprecateDays(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="structured deprecate max entries"
                value={retentionStructuredDeprecateMaxEntries}
                onChange={(e) => setRetentionStructuredDeprecateMaxEntries(e.target.value)}
              />
            </div>
            <label className="flex items-center gap-2 text-[11px] text-white/70">
              <input type="checkbox" checked={retentionDryRun} onChange={(e) => setRetentionDryRun(e.target.checked)} />
              <span>Dry run (preview only)</span>
            </label>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!agentId || retentionBusy || selectedDeployments.length === 0}
                onClick={() => void runRetention()}
              >
                {retentionBusy ? "Running…" : "Enforce retention"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={retentionBusy}
                onClick={() => {
                  setRetentionError(null);
                  setRetentionResults(null);
                }}
              >
                Clear
              </button>
            </div>
            {retentionError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {retentionError}
              </div>
            ) : null}
            {retentionResults && retentionResults.length > 0 ? (
              <div className="grid gap-2">
                {retentionResults.map((row) => {
                  const depId = String(row?.deployment_id || "");
                  const status = row?.status;
                  const ok = row?.data?.ok === true;
                  const err = row?.data?.error || row?.data?.err;
                  const deprecated = row?.data?.structured_deprecated_count;
                  return (
                    <div
                      key={`retention-${depId}`}
                      className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-xs text-white/90">
                        {depId} · {ok ? "ok" : "error"} · http {status}
                      </div>
                      <div className="text-[11px] text-white/50">
                        {typeof deprecated === "number" ? `deprecated ${deprecated}` : "no deprecations"}
                        {err ? ` · ${String(err)}` : ""}
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : null}
          </div>

          <div className="mt-4 grid gap-3">
            <div className="text-xs font-semibold text-white/80">Recaps</div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="list limit"
                value={recapsLimit}
                onChange={(e) => setRecapsLimit(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="summary max chars"
                value={recapsSummaryMaxChars}
                onChange={(e) => setRecapsSummaryMaxChars(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="recap kind (optional)"
                value={recapsKind}
                onChange={(e) => setRecapsKind(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="filter kind (list)"
                value={recapsKindFilter}
                onChange={(e) => setRecapsKindFilter(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="model override (optional)"
                value={recapsModel}
                onChange={(e) => setRecapsModel(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="daily days"
                value={recapsDailyDays}
                onChange={(e) => setRecapsDailyDays(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="max items"
                value={recapsMaxItems}
                onChange={(e) => setRecapsMaxItems(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="structured max items"
                value={recapsStructuredMaxItems}
                onChange={(e) => setRecapsStructuredMaxItems(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="daily max items"
                value={recapsDailyMaxItems}
                onChange={(e) => setRecapsDailyMaxItems(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="half-life days"
                value={recapsHalfLifeDays}
                onChange={(e) => setRecapsHalfLifeDays(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="importance weight"
                value={recapsImportanceWeight}
                onChange={(e) => setRecapsImportanceWeight(e.target.value)}
              />
            </div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <label className="flex items-center gap-2 text-[11px] text-white/70">
                <input type="checkbox" checked={recapsIncludeSummary} onChange={(e) => setRecapsIncludeSummary(e.target.checked)} />
                <span>Include summary in list</span>
              </label>
              <label className="flex items-center gap-2 text-[11px] text-white/70">
                <input type="checkbox" checked={recapsDryRun} onChange={(e) => setRecapsDryRun(e.target.checked)} />
                <span>Dry run</span>
              </label>
              <label className="flex items-center gap-2 text-[11px] text-white/70">
                <input type="checkbox" checked={recapsWriteFile} onChange={(e) => setRecapsWriteFile(e.target.checked)} />
                <span>Write recap file</span>
              </label>
              <label className="flex items-center gap-2 text-[11px] text-white/70">
                <input type="checkbox" checked={recapsIncludeStructured} onChange={(e) => setRecapsIncludeStructured(e.target.checked)} />
                <span>Include structured</span>
              </label>
              <label className="flex items-center gap-2 text-[11px] text-white/70">
                <input type="checkbox" checked={recapsIncludeDaily} onChange={(e) => setRecapsIncludeDaily(e.target.checked)} />
                <span>Include daily</span>
              </label>
            </div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!agentId || recapsListBusy || selectedDeployments.length === 0}
                onClick={() => void runRecapsList()}
              >
                {recapsListBusy ? "Loading…" : "List recaps"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!agentId || recapsGenerateBusy || selectedDeployments.length === 0}
                onClick={() => void runRecapsGenerate()}
              >
                {recapsGenerateBusy ? "Generating…" : "Generate recap"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={recapsListBusy || recapsGenerateBusy}
                onClick={() => {
                  setRecapsError(null);
                  setRecapsResults(null);
                }}
              >
                Clear
              </button>
            </div>
            {recapsError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {recapsError}
              </div>
            ) : null}
            {recapsResults && recapsResults.length > 0 ? (
              <div className="grid gap-2">
                {recapsResults.map((row) => {
                  const depId = String(row?.deployment_id || "");
                  const status = row?.status;
                  const ok = row?.data?.ok === true;
                  const err = row?.data?.error || row?.data?.err;
                  const count = row?.data?.count;
                  return (
                    <div
                      key={`recaps-${depId}`}
                      className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-xs text-white/90">
                        {depId} · {ok ? "ok" : "error"} · http {status}
                      </div>
                      <div className="text-[11px] text-white/50">
                        {typeof count === "number" ? `count ${count}` : "no count"}
                        {err ? ` · ${String(err)}` : ""}
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : null}
          </div>

          <div className="mt-4 grid gap-3">
            <div className="text-xs font-semibold text-white/80">Salience (uses recaps tuning)</div>
            <div className="text-[11px] text-white/50">Pulls salience with the same limits/weights above.</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!agentId || salienceBusy || selectedDeployments.length === 0}
                onClick={() => void runSalience()}
              >
                {salienceBusy ? "Fetching…" : "Fetch salience"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={salienceBusy}
                onClick={() => {
                  setSalienceError(null);
                  setSalienceResults(null);
                }}
              >
                Clear
              </button>
            </div>
            {salienceError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {salienceError}
              </div>
            ) : null}
            {salienceResults && salienceResults.length > 0 ? (
              <div className="grid gap-2">
                {salienceResults.map((row) => {
                  const depId = String(row?.deployment_id || "");
                  const status = row?.status;
                  const ok = row?.data?.ok === true;
                  const err = row?.data?.error || row?.data?.err;
                  const structuredCount = Array.isArray(row?.data?.structured_items)
                    ? row.data.structured_items.length
                    : null;
                  const dailyCount = Array.isArray(row?.data?.daily_items) ? row.data.daily_items.length : null;
                  const returned = row?.data?.returned;
                  return (
                    <div
                      key={`salience-${depId}`}
                      className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-xs text-white/90">
                        {depId} · {ok ? "ok" : "error"} · http {status}
                      </div>
                      <div className="text-[11px] text-white/50">
                        {typeof returned === "number" ? `returned ${returned}` : "no count"}
                        {structuredCount !== null ? ` · structured ${structuredCount}` : ""}
                        {dailyCount !== null ? ` · daily ${dailyCount}` : ""}
                        {err ? ` · ${String(err)}` : ""}
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : null}
          </div>
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Membership audit</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || auditQuery.isFetching}
              onClick={() => void auditQuery.refetch()}
            >
              {auditQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>
          <div className="mb-2 flex flex-wrap items-center gap-2">
            <FieldLabel>Limit</FieldLabel>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={auditLimit}
              onChange={(e) => setAuditLimit(e.target.value)}
            />
            <span className="text-[11px] text-white/50">(1-500)</span>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to view audit history.</div>
          ) : auditQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(auditQuery.error)}
            </div>
          ) : auditRows.length === 0 ? (
            <div className="text-[11px] text-white/50">No audit rows.</div>
          ) : (
            <div className="grid gap-2">
              {auditRows.map((row, idx) => {
                const action = String(row?.action || "");
                const actor = String(row?.actor_sub || "");
                const target = String(row?.target_sub || "");
                const role = String(row?.role || "");
                const traceId = String(row?.trace_id || "");
                const ts = fmtTs(row?.ts_unix_ms);
                return (
                  <div
                    key={`${actor}-${target}-${action}-${idx}`}
                    className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <div className="text-xs text-white/90">{action || "update"}</div>
                    <div className="text-[11px] text-white/50">
                      actor {actor} → target {target}
                      {role ? ` · role ${role}` : ""}
                      {traceId ? ` · trace ${traceId}` : ""}
                      {ts ? ` · ${ts}` : ""}
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </section>

        <BrokerTeamConsole base={base} auth={props.auth} quorumEvents={brokerEvents} />

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Live broker events</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => setBrokerEventsActive((prev) => !prev)}
              >
                {brokerEventsActive ? "Pause" : "Resume"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => setBrokerEvents([])}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="mb-2 flex flex-wrap items-center gap-3 text-[11px] text-white/50">
            <span>
              {brokerEventsActive ? (brokerEventsConnected ? "Connected" : "Connecting…") : "Paused"}
            </span>
            <label className="flex items-center gap-1">
              <input
                type="checkbox"
                checked={brokerEventsQuorumOnly}
                onChange={(e) => setBrokerEventsQuorumOnly(e.target.checked)}
              />
              Quorum only
            </label>
            <span>{brokerEvents.length} events</span>
          </div>
          {brokerEventsError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {brokerEventsError}
            </div>
          ) : brokerEventRows.length === 0 ? (
            <div className="text-[11px] text-white/50">No events yet.</div>
          ) : (
            <div className="grid gap-2">
              {brokerEventRows.map((row, idx) => {
                const type = String(row?.type || "");
                const payload = row?.payload ?? {};
                const ts = fmtTs(row?.ts_unix_ms);
                const traceId = row?.trace_id ? String(row.trace_id) : "";
                let summary = "";
                if (type === "team_quorum_request") {
                  const action = payload?.action ? String(payload.action) : "";
                  const min = payload?.min_approvals;
                  const ruleId = payload?.rule_id ? String(payload.rule_id) : "";
                  summary = `${action || "quorum"} · min ${min ?? "?"}${ruleId ? ` · rule ${ruleId}` : ""}`;
                } else if (type === "team_quorum_result") {
                  const decision = payload?.decision ? String(payload.decision) : "result";
                  const approvals = payload?.approvals;
                  const required = payload?.required_approvals;
                  summary = `${decision} · ${approvals ?? "?"}/${required ?? "?"}`;
                } else if (payload && Object.keys(payload).length > 0) {
                  try {
                    summary = JSON.stringify(payload);
                  } catch {
                    summary = String(payload);
                  }
                  if (summary.length > 120) summary = `${summary.slice(0, 117)}…`;
                }
                return (
                  <div
                    key={`broker-event-${type}-${idx}`}
                    className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <div className="text-xs text-white/90">{type || "event"}</div>
                    <div className="text-[11px] text-white/50">
                      {summary || "payload captured"}
                      {traceId ? ` · trace ${traceId}` : ""}
                      {ts ? ` · ${ts}` : ""}
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </section>
      </div>
    </details>
  );
}
