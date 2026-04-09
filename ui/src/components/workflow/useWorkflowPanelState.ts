import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiCancelWorkflow,
  apiCreateWorkflowSchedule,
  apiDeleteWorkflowSchedule,
  apiGetWorkflow,
  apiListWorkflowScheduleRuns,
  apiListWorkflowSchedules,
  apiListWorkflows,
  apiPauseWorkflowSchedule,
  apiResumeWorkflowSchedule,
  type WorkflowDetailResp,
} from "../../api";
import type { ApiAuth } from "../../api/auth";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import type {
  WorkflowScheduleCreatePayload,
  WorkflowScheduleRow,
  WorkflowScheduleRunRow,
  WorkflowSpec,
  WorkflowSummaryRow,
} from "../../workflowTypes";
import {
  buildLevels,
  coerceScheduleSpec,
  countByStatus,
  extractScheduleRuns,
  extractSchedules,
  extractTasks,
  extractWorkflowLimits,
  extractWorkflowRemaining,
  extractWorkflowSummary,
  extractWorkflowUsage,
  extractWorkflows,
  SCHEDULE_RUN_STATUS_OPTIONS,
  SCHEDULE_STATUS_OPTIONS,
  STATUS_OPTIONS,
  validateCronExpr,
  validateScheduleSpec,
} from "./workflowPanelUtils";

type UseWorkflowPanelStateArgs = {
  open: boolean;
  baseUrl: string;
  auth?: ApiAuth;
  authKey?: string;
};

const isObjectRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === "object" && !Array.isArray(value);

export default function useWorkflowPanelState(args: UseWorkflowPanelStateArgs) {
  const [workflowId, setWorkflowId] = useLocalStorageState("agentui.workflowLookupId", "");
  const [listStatus, setListStatus] = useLocalStorageState("agentui.workflowListStatus", "active");
  const [listLimit, setListLimit] = useLocalStorageState("agentui.workflowListLimit", "50");
  const [listFilter, setListFilter] = useLocalStorageState("agentui.workflowListFilter", "");
  const [listFilterDebounced, setListFilterDebounced] = React.useState(listFilter);
  const [listAutoRefresh, setListAutoRefresh] = useLocalStorageState("agentui.workflowListAutoRefresh", false);
  const [includeResults, setIncludeResults] = useLocalStorageState("agentui.workflowIncludeResults", false);
  const [includeSpec, setIncludeSpec] = useLocalStorageState("agentui.workflowIncludeSpec", false);
  const [detail, setDetail] = React.useState<WorkflowDetailResp | null>(null);
  const [detailError, setDetailError] = React.useState<string | null>(null);
  const [cancelBusyId, setCancelBusyId] = React.useState<string | null>(null);
  const [scheduleStatus, setScheduleStatus] = useLocalStorageState("agentui.workflowScheduleStatus", "active");
  const [scheduleLimit, setScheduleLimit] = useLocalStorageState("agentui.workflowScheduleLimit", "50");
  const [scheduleOffset, setScheduleOffset] = useLocalStorageState("agentui.workflowScheduleOffset", "0");
  const [scheduleFilter, setScheduleFilter] = useLocalStorageState("agentui.workflowScheduleFilter", "");
  const [scheduleAutoRefresh, setScheduleAutoRefresh] = useLocalStorageState("agentui.workflowScheduleAutoRefresh", false);
  const [scheduleCron, setScheduleCron] = useLocalStorageState("agentui.workflowScheduleCron", "0 9 * * 1-5");
  const [scheduleSpec, setScheduleSpec] = useLocalStorageState("agentui.workflowScheduleSpec", "");
  const [scheduleId, setScheduleId] = useLocalStorageState("agentui.workflowScheduleId", "");
  const [scheduleRunsLimit, setScheduleRunsLimit] = useLocalStorageState("agentui.workflowScheduleRunsLimit", "50");
  const [scheduleRunsOffset, setScheduleRunsOffset] = useLocalStorageState("agentui.workflowScheduleRunsOffset", "0");
  const [scheduleRunsStatus, setScheduleRunsStatus] = useLocalStorageState("agentui.workflowScheduleRunsStatus", "all");
  const [scheduleRunsErrorsOnly, setScheduleRunsErrorsOnly] = useLocalStorageState(
    "agentui.workflowScheduleRunsErrorsOnly",
    false,
  );
  const [scheduleRunsFilter, setScheduleRunsFilter] = useLocalStorageState("agentui.workflowScheduleRunsFilter", "");
  const [scheduleError, setScheduleError] = React.useState<string | null>(null);
  const [scheduleValidation, setScheduleValidation] = React.useState<string[]>([]);
  const [scheduleCronValidation, setScheduleCronValidation] = React.useState<string[]>([]);
  const [scheduleBusyId, setScheduleBusyId] = React.useState<string | null>(null);
  const [scheduleCreateBusy, setScheduleCreateBusy] = React.useState(false);
  const [copyNotice, setCopyNotice] = React.useState<string | null>(null);
  const copyTimerRef = React.useRef<number | null>(null);

  const normalizedListStatus = STATUS_OPTIONS.includes(String(listStatus)) ? String(listStatus) : "running";
  const limitValue = (() => {
    const n = Number(listLimit);
    if (!Number.isFinite(n)) return 50;
    return Math.min(Math.max(Math.trunc(n), 1), 200);
  })();
  const normalizedScheduleStatus = SCHEDULE_STATUS_OPTIONS.includes(String(scheduleStatus))
    ? String(scheduleStatus)
    : "active";
  const scheduleLimitValue = (() => {
    const n = Number(scheduleLimit);
    if (!Number.isFinite(n)) return 50;
    return Math.min(Math.max(Math.trunc(n), 1), 200);
  })();
  const scheduleOffsetValue = (() => {
    const n = Number(scheduleOffset);
    if (!Number.isFinite(n) || n < 0) return 0;
    return Math.trunc(n);
  })();
  const scheduleRunsLimitValue = (() => {
    const n = Number(scheduleRunsLimit);
    if (!Number.isFinite(n)) return 50;
    return Math.min(Math.max(Math.trunc(n), 1), 200);
  })();
  const scheduleRunsOffsetValue = (() => {
    const n = Number(scheduleRunsOffset);
    if (!Number.isFinite(n) || n < 0) return 0;
    return Math.trunc(n);
  })();
  const normalizedScheduleRunsStatus = SCHEDULE_RUN_STATUS_OPTIONS.includes(String(scheduleRunsStatus))
    ? String(scheduleRunsStatus)
    : "all";

  const listQuery = useQuery({
    queryKey: ["workflows", args.baseUrl, args.authKey, normalizedListStatus, limitValue, listFilterDebounced],
    queryFn: () =>
      apiListWorkflows(
        args.baseUrl,
        { status: normalizedListStatus, limit: limitValue, query: listFilterDebounced },
        args.auth,
      ),
    enabled: args.open && !!args.baseUrl,
    staleTime: 5_000,
    refetchInterval: listAutoRefresh ? 5_000 : false,
  });

  const scheduleListQuery = useQuery({
    queryKey: [
      "workflow-schedules",
      args.baseUrl,
      args.authKey,
      normalizedScheduleStatus,
      scheduleLimitValue,
      scheduleOffsetValue,
    ],
    queryFn: () =>
      apiListWorkflowSchedules(
        args.baseUrl,
        {
          status: normalizedScheduleStatus === "all" ? undefined : normalizedScheduleStatus,
          limit: scheduleLimitValue,
          offset: scheduleOffsetValue,
        },
        args.auth,
      ),
    enabled: args.open && !!args.baseUrl,
    staleTime: 5_000,
    refetchInterval: scheduleAutoRefresh ? 5_000 : false,
  });

  const scheduleRunsQuery = useQuery({
    queryKey: [
      "workflow-schedule-runs",
      args.baseUrl,
      args.authKey,
      scheduleId,
      scheduleRunsLimitValue,
      scheduleRunsOffsetValue,
    ],
    queryFn: () =>
      apiListWorkflowScheduleRuns(
        args.baseUrl,
        { scheduleId: String(scheduleId || ""), limit: scheduleRunsLimitValue, offset: scheduleRunsOffsetValue },
        args.auth,
      ),
    enabled: args.open && !!args.baseUrl && String(scheduleId || "").trim().length > 0,
    staleTime: 5_000,
    refetchInterval: scheduleAutoRefresh ? 5_000 : false,
  });

  React.useEffect(() => {
    const next = String(listFilter || "").trim();
    const handle = window.setTimeout(() => {
      setListFilterDebounced(next);
    }, 300);
    return () => window.clearTimeout(handle);
  }, [listFilter]);

  const workflowLookup = useMutation({
    mutationFn: async (id: string) =>
      apiGetWorkflow(
        args.baseUrl,
        {
          workflowId: id,
          includeTasks: true,
          includeResults,
          includeSpec,
        },
        args.auth,
      ),
    onSuccess: (resp) => {
      setDetail(resp);
      setDetailError(resp && resp.ok === false ? resp.error || "workflow lookup failed" : null);
    },
    onError: (err) => {
      setDetail(null);
      setDetailError(String(err));
    },
  });

  const filteredWorkflows = React.useMemo(() => {
    const workflows = extractWorkflows(listQuery.data);
    const query = String(listFilter || "").trim().toLowerCase();
    if (!query) return workflows;
    return workflows.filter((wf: WorkflowSummaryRow) => {
      const workflowId = String(wf.workflow_id || "").toLowerCase();
      const traceId = String(wf.trace_id || "").toLowerCase();
      const sessionId = String(wf.session_id || "").toLowerCase();
      const idempotencyKey = String(wf.idempotency_key || "").toLowerCase();
      return (
        workflowId.includes(query) ||
        traceId.includes(query) ||
        sessionId.includes(query) ||
        idempotencyKey.includes(query)
      );
    });
  }, [listFilter, listQuery.data]);

  const tasks = extractTasks(detail);
  const summary = extractWorkflowSummary(detail);
  const taskCounts = countByStatus(tasks);
  const graph = buildLevels(tasks);
  const workflowLimits = extractWorkflowLimits(detail);
  const workflowUsage = extractWorkflowUsage(detail);
  const workflowRemaining = extractWorkflowRemaining(detail);
  const scheduleList = extractSchedules(scheduleListQuery.data);
  const scheduleRuns = extractScheduleRuns(scheduleRunsQuery.data);

  const filteredScheduleList = React.useMemo(() => {
    const query = String(scheduleFilter || "").trim().toLowerCase();
    if (!query) return scheduleList;
    return scheduleList.filter((sched: WorkflowScheduleRow) => {
      const localScheduleId = String(sched?.schedule_id || "").toLowerCase();
      const cron = String(sched?.cron || "").toLowerCase();
      const timezone = String(sched?.timezone || "").toLowerCase();
      const lastError = String(sched?.last_error || "").toLowerCase();
      return (
        localScheduleId.includes(query) ||
        cron.includes(query) ||
        timezone.includes(query) ||
        lastError.includes(query)
      );
    });
  }, [scheduleList, scheduleFilter]);

  const filteredScheduleRuns = React.useMemo(() => {
    const statusFilter = normalizedScheduleRunsStatus === "all" ? "" : normalizedScheduleRunsStatus;
    const query = String(scheduleRunsFilter || "").trim().toLowerCase();
    return scheduleRuns.filter((run: WorkflowScheduleRunRow) => {
      const status = String(run?.status || "").toLowerCase();
      if (statusFilter && status !== statusFilter) return false;
      if (query) {
        const workflowId = String(run?.workflow_id || "").toLowerCase();
        const localScheduleId = String(run?.schedule_id || "").toLowerCase();
        if (!workflowId.includes(query) && !localScheduleId.includes(query)) return false;
      }
      if (scheduleRunsErrorsOnly && !String(run?.error || "").trim()) return false;
      return true;
    });
  }, [scheduleRuns, normalizedScheduleRunsStatus, scheduleRunsErrorsOnly, scheduleRunsFilter]);

  const loadWorkflow = React.useCallback(
    (id: string) => {
      const trimmed = String(id || "").trim();
      if (!trimmed) return;
      if (!args.baseUrl) {
        setDetailError("Base URL is not set.");
        return;
      }
      setDetailError(null);
      workflowLookup.mutate(trimmed);
    },
    [args.baseUrl, workflowLookup],
  );

  const loadScheduleRuns = React.useCallback((id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    setScheduleId(trimmed);
  }, []);

  const clearWorkflow = React.useCallback(() => {
    setDetail(null);
    setDetailError(null);
  }, []);

  const cancelWorkflow = async (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!args.baseUrl) {
      setDetailError("Base URL is not set.");
      return;
    }
    setCancelBusyId(trimmed);
    setDetailError(null);
    try {
      const resp = await apiCancelWorkflow(args.baseUrl, trimmed, args.auth);
      if (resp && resp.ok === false) {
        setDetailError(resp.error || "Workflow cancel failed");
      }
    } catch (err) {
      setDetailError(String(err));
    } finally {
      setCancelBusyId(null);
    }
    loadWorkflow(trimmed);
    void listQuery.refetch();
  };

  const createSchedule = async () => {
    if (!args.baseUrl) {
      setScheduleError("Base URL is not set.");
      return;
    }
    const cron = String(scheduleCron || "").trim();
    const cronIssues = validateCronExpr(cron);
    setScheduleCronValidation(cronIssues);
    if (cronIssues.length > 0) {
      setScheduleError("cron validation failed");
      return;
    }
    const specRaw = String(scheduleSpec || "").trim();
    if (!specRaw) {
      setScheduleError("spec JSON is required");
      return;
    }
    let specObj: unknown = null;
    try {
      specObj = JSON.parse(specRaw);
    } catch (err) {
      setScheduleError(`spec JSON parse error: ${String(err)}`);
      return;
    }
    const issues = validateScheduleSpec(specObj);
    setScheduleValidation(issues);
    if (issues.length > 0) {
      setScheduleError(`spec validation failed (${issues.length} issues)`);
      return;
    }
    const spec = coerceScheduleSpec(specObj);
    if (!spec) {
      setScheduleError("spec validation failed");
      return;
    }
    setScheduleCreateBusy(true);
    setScheduleError(null);
    try {
      const resp = await apiCreateWorkflowSchedule(
        args.baseUrl,
        { cron, timezone: "UTC", spec },
        args.auth,
      );
      if (resp && resp.ok === false) {
        setScheduleError(resp.error || "schedule create failed");
      } else if (resp && resp.schedule_id) {
        setScheduleId(resp.schedule_id);
      }
      void scheduleListQuery.refetch();
      void scheduleRunsQuery.refetch();
    } catch (err) {
      setScheduleError(String(err));
    } finally {
      setScheduleCreateBusy(false);
    }
  };

  const scheduleCurlSnippet = (id: string, action: "pause" | "resume" | "delete") => {
    const base = String(args.baseUrl || "").replace(/\/$/, "");
    const token = "$AGENTD_AUTH_TOKEN";
    if (action === "delete") {
      return `curl -H "Authorization: Bearer ${token}" -X DELETE ${base}/api/v1/workflow_schedule?schedule_id=${encodeURIComponent(
        id,
      )}`;
    }
    return `curl -H "Authorization: Bearer ${token}" -H "Content-Type: application/json" -d '{"schedule_id":"${id}"}' ${base}/api/v1/workflow_schedule/${action}`;
  };

  const scheduleCreateCurlSnippet = (cron: string, spec: WorkflowScheduleCreatePayload["spec"]) => {
    const base = String(args.baseUrl || "").replace(/\/$/, "");
    const token = "$AGENTD_AUTH_TOKEN";
    const payload = JSON.stringify({ cron, timezone: "UTC", spec });
    return `curl -H "Authorization: Bearer ${token}" -H "Content-Type: application/json" -d '${payload}' ${base}/api/v1/workflow_schedules`;
  };

  const pauseSchedule = async (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!args.baseUrl) {
      setScheduleError("Base URL is not set.");
      return;
    }
    setScheduleBusyId(trimmed);
    setScheduleError(null);
    try {
      const resp = await apiPauseWorkflowSchedule(args.baseUrl, trimmed, args.auth);
      if (resp && resp.ok === false) setScheduleError(resp.error || "schedule pause failed");
    } catch (err) {
      setScheduleError(String(err));
    } finally {
      setScheduleBusyId(null);
    }
    void scheduleListQuery.refetch();
  };

  const resumeSchedule = async (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!args.baseUrl) {
      setScheduleError("Base URL is not set.");
      return;
    }
    setScheduleBusyId(trimmed);
    setScheduleError(null);
    try {
      const resp = await apiResumeWorkflowSchedule(args.baseUrl, trimmed, args.auth);
      if (resp && resp.ok === false) setScheduleError(resp.error || "schedule resume failed");
    } catch (err) {
      setScheduleError(String(err));
    } finally {
      setScheduleBusyId(null);
    }
    void scheduleListQuery.refetch();
  };

  const deleteSchedule = async (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!args.baseUrl) {
      setScheduleError("Base URL is not set.");
      return;
    }
    setScheduleBusyId(trimmed);
    setScheduleError(null);
    try {
      const resp = await apiDeleteWorkflowSchedule(args.baseUrl, trimmed, args.auth);
      if (resp && resp.ok === false) setScheduleError(resp.error || "schedule delete failed");
    } catch (err) {
      setScheduleError(String(err));
    } finally {
      setScheduleBusyId(null);
    }
    if (String(scheduleId || "") === trimmed) setScheduleId("");
    void scheduleListQuery.refetch();
    void scheduleRunsQuery.refetch();
  };

  const loadSpecFromWorkflow = React.useCallback(() => {
    if (detail?.spec_json) {
      setScheduleSpec(detail.spec_json);
      try {
        const parsed = JSON.parse(detail.spec_json);
        setScheduleValidation(validateScheduleSpec(parsed));
      } catch {
        setScheduleValidation([]);
      }
      return;
    }
    if (isObjectRecord(detail?.spec)) {
      setScheduleSpec(JSON.stringify(detail.spec as WorkflowSpec, null, 2));
      setScheduleValidation(validateScheduleSpec(detail.spec));
    }
  }, [detail, setScheduleSpec]);

  const copyText = async (label: string, value?: string | null) => {
    const text = String(value || "").trim();
    if (!text) return;
    try {
      if (navigator?.clipboard?.writeText) {
        await navigator.clipboard.writeText(text);
      } else {
        const textarea = document.createElement("textarea");
        textarea.value = text;
        textarea.style.position = "fixed";
        textarea.style.opacity = "0";
        document.body.appendChild(textarea);
        textarea.focus();
        textarea.select();
        document.execCommand("copy");
        document.body.removeChild(textarea);
      }
      setCopyNotice(`${label} copied`);
    } catch {
      setCopyNotice(`Failed to copy ${label}`);
    }
    if (copyTimerRef.current) {
      window.clearTimeout(copyTimerRef.current);
    }
    copyTimerRef.current = window.setTimeout(() => setCopyNotice(null), 2000);
  };

  const copyJson = async (label: string, payload: unknown) => {
    try {
      const text = JSON.stringify(payload, null, 2);
      await copyText(label, text);
    } catch {
      setCopyNotice(`Failed to copy ${label}`);
      if (copyTimerRef.current) window.clearTimeout(copyTimerRef.current);
      copyTimerRef.current = window.setTimeout(() => setCopyNotice(null), 2000);
    }
  };

  const downloadJson = (label: string, payload: unknown) => {
    try {
      const text = JSON.stringify(payload, null, 2);
      const blob = new Blob([text], { type: "application/json" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = `${label}-${Date.now()}.json`;
      a.click();
      URL.revokeObjectURL(url);
    } catch {
      setCopyNotice(`Failed to download ${label}`);
      if (copyTimerRef.current) window.clearTimeout(copyTimerRef.current);
      copyTimerRef.current = window.setTimeout(() => setCopyNotice(null), 2000);
    }
  };

  React.useEffect(
    () => () => {
      if (copyTimerRef.current) window.clearTimeout(copyTimerRef.current);
    },
    [],
  );

  return {
    workflowId,
    setWorkflowId,
    listStatus,
    setListStatus,
    listLimit,
    setListLimit,
    listFilter,
    setListFilter,
    listAutoRefresh,
    setListAutoRefresh,
    includeResults,
    setIncludeResults,
    includeSpec,
    setIncludeSpec,
    detail,
    detailError,
    cancelBusyId,
    scheduleStatus,
    setScheduleStatus,
    scheduleLimit,
    setScheduleLimit,
    scheduleOffset,
    setScheduleOffset,
    scheduleFilter,
    setScheduleFilter,
    scheduleAutoRefresh,
    setScheduleAutoRefresh,
    scheduleCron,
    setScheduleCron,
    scheduleSpec,
    setScheduleSpec,
    scheduleId,
    scheduleRunsLimit,
    setScheduleRunsLimit,
    scheduleRunsOffset,
    setScheduleRunsOffset,
    scheduleRunsStatus,
    setScheduleRunsStatus,
    scheduleRunsErrorsOnly,
    setScheduleRunsErrorsOnly,
    scheduleRunsFilter,
    setScheduleRunsFilter,
    scheduleError,
    setScheduleError,
    scheduleValidation,
    setScheduleValidation,
    scheduleCronValidation,
    setScheduleCronValidation,
    scheduleBusyId,
    scheduleCreateBusy,
    copyNotice,
    normalizedListStatus,
    limitValue,
    normalizedScheduleStatus,
    scheduleLimitValue,
    scheduleOffsetValue,
    scheduleRunsLimitValue,
    scheduleRunsOffsetValue,
    normalizedScheduleRunsStatus,
    listQuery,
    scheduleListQuery,
    scheduleRunsQuery,
    workflowLookup,
    filteredWorkflows,
    tasks,
    summary,
    taskCounts,
    graph,
    workflowLimits,
    workflowUsage,
    workflowRemaining,
    scheduleList,
    scheduleRuns,
    filteredScheduleList,
    filteredScheduleRuns,
    canLoad: String(workflowId || "").trim().length > 0,
    loadWorkflow,
    clearWorkflow,
    loadScheduleRuns,
    cancelWorkflow,
    createSchedule,
    pauseSchedule,
    resumeSchedule,
    deleteSchedule,
    loadSpecFromWorkflow,
    copyText,
    copyJson,
    downloadJson,
    scheduleCurlSnippet,
    scheduleCreateCurlSnippet,
  };
}
