import React from "react";
import {
  apiGetApproval,
  apiListApprovals,
  apiPostApprovalDecision,
  type ApiAuth,
  type ApprovalDecision,
  type ApprovalRequest,
} from "../../api";
import type { ApprovalQueueState } from "./approvalQueueTypes";

type UseApprovalQueueStateArgs = {
  auth: ApiAuth;
  baseUrl: string;
};

export function useApprovalQueueState(args: UseApprovalQueueStateArgs): ApprovalQueueState {
  const base = String(args.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;

  const [statusFilter, setStatusFilter] = React.useState<string>("");
  const [teamIdFilter, setTeamIdFilter] = React.useState<string>("");
  const [traceIdFilter, setTraceIdFilter] = React.useState<string>("");
  const [jobIdFilter, setJobIdFilter] = React.useState<string>("");
  const [toolNameFilter, setToolNameFilter] = React.useState<string>("");
  const [runIdFilter, setRunIdFilter] = React.useState<string>("");
  const [limitFilter, setLimitFilter] = React.useState<string>("100");

  const [approvals, setApprovals] = React.useState<ApprovalRequest[]>([]);
  const [listError, setListError] = React.useState<string | null>(null);
  const [listBusy, setListBusy] = React.useState<boolean>(false);

  const [selectedId, setSelectedId] = React.useState<string | null>(null);
  const [detailApproval, setDetailApproval] = React.useState<ApprovalRequest | null>(null);
  const [detailDecisions, setDetailDecisions] = React.useState<ApprovalDecision[]>([]);
  const [detailBusy, setDetailBusy] = React.useState<boolean>(false);
  const [detailError, setDetailError] = React.useState<string | null>(null);

  const [memberId, setMemberId] = React.useState<string>("");
  const [memberRole, setMemberRole] = React.useState<string>("");
  const [note, setNote] = React.useState<string>("");
  const [decisionBusy, setDecisionBusy] = React.useState<boolean>(false);
  const [decisionError, setDecisionError] = React.useState<string | null>(null);

  const parseLimit = React.useCallback((raw: string, fallback = 100) => {
    const n = Number.parseInt(String(raw || "").trim(), 10);
    if (!Number.isFinite(n) || n <= 0) return fallback;
    return Math.min(n, 500);
  }, []);

  const loadDetail = React.useCallback(
    async (approvalId: string) => {
      setDetailError(null);
      setDetailBusy(true);
      try {
        const res = await apiGetApproval(base, approvalId, args.auth);
        if (!res.ok) throw new Error(res.error || "failed to load approval");
        setDetailApproval(res.approval ?? null);
        setDetailDecisions(res.decisions ?? []);
        const roles = Array.isArray(res.approval?.role_constraints) ? res.approval?.role_constraints ?? [] : [];
        if (roles.length > 0) {
          const cur = memberRole.trim();
          if (!cur) setMemberRole(String(roles[0] ?? ""));
        }
      } catch (err) {
        setDetailError(String(err));
        setDetailApproval(null);
        setDetailDecisions([]);
      } finally {
        setDetailBusy(false);
      }
    },
    [args.auth, base, memberRole],
  );

  const loadApprovals = React.useCallback(async () => {
    setListError(null);
    setListBusy(true);
    try {
      if (!canQuery) throw new Error("missing base URL");
      const res = await apiListApprovals(
        base,
        {
          status: statusFilter.trim() || undefined,
          teamId: teamIdFilter.trim() || undefined,
          traceId: traceIdFilter.trim() || undefined,
          jobId: jobIdFilter.trim() || undefined,
          toolName: toolNameFilter.trim() || undefined,
          runId: runIdFilter.trim() || undefined,
          limit: parseLimit(limitFilter),
        },
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "failed to load approvals");
      setApprovals(res.approvals ?? []);
    } catch (err) {
      setListError(String(err));
    } finally {
      setListBusy(false);
    }
  }, [args.auth, base, canQuery, jobIdFilter, limitFilter, parseLimit, runIdFilter, statusFilter, teamIdFilter, toolNameFilter, traceIdFilter]);

  const clearApprovals = React.useCallback(() => {
    setApprovals([]);
    setListError(null);
  }, []);

  const selectApproval = React.useCallback(
    (approvalId: string) => {
      setSelectedId(approvalId);
      setDecisionError(null);
      setMemberId("");
      setMemberRole("");
      setNote("");
      void loadDetail(approvalId);
    },
    [loadDetail],
  );

  const submitDecision = React.useCallback(
    async (decision: "approve" | "deny") => {
      if (!selectedId) return;
      setDecisionError(null);
      setDecisionBusy(true);
      try {
        const m = memberId.trim();
        if (!m) throw new Error("missing member_id");
        const roleConstraints = Array.isArray(detailApproval?.role_constraints) ? detailApproval?.role_constraints ?? [] : [];
        const role = memberRole.trim();
        if (roleConstraints.length > 0 && !role) throw new Error("missing member_role");
        const res = await apiPostApprovalDecision(
          base,
          selectedId,
          { memberId: m, memberRole: role || undefined, decision, note: note.trim() || undefined },
          args.auth,
        );
        if (!res.ok) throw new Error(res.error || "decision failed");
        await loadDetail(selectedId);
        await loadApprovals();
      } catch (err) {
        setDecisionError(String(err));
      } finally {
        setDecisionBusy(false);
      }
    },
    [args.auth, base, detailApproval?.role_constraints, loadApprovals, loadDetail, memberId, memberRole, note, selectedId],
  );

  const selectedStatus = detailApproval?.status || "";
  const isPending = selectedStatus === "pending" || !selectedStatus;

  return {
    approvals,
    auth: args.auth,
    base,
    canQuery,
    clearApprovals,
    decisionBusy,
    decisionError,
    detailApproval,
    detailBusy,
    detailDecisions,
    detailError,
    isPending,
    jobIdFilter,
    limitFilter,
    listBusy,
    listError,
    loadApprovals,
    memberId,
    memberRole,
    note,
    runIdFilter,
    selectedId,
    selectApproval,
    setJobIdFilter,
    setLimitFilter,
    setMemberId,
    setMemberRole,
    setNote,
    setRunIdFilter,
    setStatusFilter,
    setTeamIdFilter,
    setToolNameFilter,
    setTraceIdFilter,
    statusFilter,
    submitDecision,
    teamIdFilter,
    toolNameFilter,
    traceIdFilter,
  };
}
