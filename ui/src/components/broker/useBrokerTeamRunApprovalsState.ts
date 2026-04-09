import React from "react";

import {
  apiBrokerTeamRunApprovalsCreate,
  apiBrokerTeamRunApprovalsList,
  type ApiAuth,
} from "../../api";
import type { TeamRunApprovalRow } from "./teamRunPanelTypes";
import type { TeamRunCreateResult, TeamRunLookupResult } from "./teamRunStatusTypes";

type UseBrokerTeamRunApprovalsStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  runLookupId: string;
  runLookupResult: TeamRunLookupResult | null;
  runResult: TeamRunCreateResult | null;
};

export default function useBrokerTeamRunApprovalsState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  runLookupId,
  runLookupResult,
  runResult,
}: UseBrokerTeamRunApprovalsStateArgs) {
  const [approvalsBusy, setApprovalsBusy] = React.useState<boolean>(false);
  const [approvalsError, setApprovalsError] = React.useState<string | null>(null);
  const [approvals, setApprovals] = React.useState<TeamRunApprovalRow[] | null>(null);
  const [approvalsLastSyncMs, setApprovalsLastSyncMs] = React.useState<number | null>(null);
  const [approvalRunId, setApprovalRunId] = React.useState<string>("");
  const [approvalMemberId, setApprovalMemberId] = React.useState<string>("");
  const [approvalRuleId, setApprovalRuleId] = React.useState<string>("");
  const [approvalDecision, setApprovalDecision] = React.useState<string>("approve");
  const [approvalReason, setApprovalReason] = React.useState<string>("");
  const lastAutoApprovalRunIdRef = React.useRef<string>("");

  const approvalRunIdTrimmed = String(approvalRunId || runLookupId || "").trim();

  const handleApprovalsRefresh = React.useCallback(async () => {
    const runId = approvalRunIdTrimmed;
    if (!teamIdTrimmed || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsList(base, teamIdTrimmed, runId, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "approvals fetch failed");
      }
      const rows = Array.isArray(resp?.approvals) ? resp.approvals : [];
      setApprovals(rows);
      setApprovalsLastSyncMs(Date.now());
    } catch (err) {
      setApprovalsError(String(err));
    } finally {
      setApprovalsBusy(false);
    }
  }, [approvalRunIdTrimmed, auth, base, teamIdTrimmed]);

  const handleApprovalSubmit = React.useCallback(async () => {
    const runId = approvalRunIdTrimmed;
    const memberId = String(approvalMemberId || "").trim();
    const decision = String(approvalDecision || "").trim().toLowerCase();
    const ruleId = String(approvalRuleId || "").trim();
    const reason = String(approvalReason || "").trim();
    if (!teamIdTrimmed || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    if (!memberId) {
      setApprovalsError("member_id required");
      return;
    }
    if (!decision) {
      setApprovalsError("decision required");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const payload: { member_id: string; decision: string; rule_id?: string; reason?: string } = {
        member_id: memberId,
        decision,
      };
      if (ruleId) payload.rule_id = ruleId;
      if (reason) payload.reason = reason;
      const resp = await apiBrokerTeamRunApprovalsCreate(base, teamIdTrimmed, runId, payload, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "approvals update failed");
      }
      const rows = Array.isArray(resp?.approvals) ? resp.approvals : [];
      setApprovals(rows);
      setApprovalsLastSyncMs(Date.now());
      setApprovalReason("");
    } catch (err) {
      setApprovalsError(String(err));
    } finally {
      setApprovalsBusy(false);
    }
  }, [approvalDecision, approvalMemberId, approvalReason, approvalRuleId, approvalRunIdTrimmed, auth, base, teamIdTrimmed]);

  React.useEffect(() => {
    if (!approvalRunId && runResult?.team_run_id) {
      setApprovalRunId(String(runResult.team_run_id));
    }
  }, [approvalRunId, runResult]);

  React.useEffect(() => {
    if (!approvalRunId && runLookupResult?.team_run_id) {
      setApprovalRunId(String(runLookupResult.team_run_id));
    }
  }, [approvalRunId, runLookupResult]);

  React.useEffect(() => {
    setApprovals(null);
    setApprovalsError(null);
    setApprovalsLastSyncMs(null);
  }, [approvalRunIdTrimmed, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    setApprovalRunId("");
    setApprovals(null);
    setApprovalsError(null);
    lastAutoApprovalRunIdRef.current = "";
  }, [teamIdTrimmed]);

  React.useEffect(() => {
    if (!approvalRunIdTrimmed) {
      lastAutoApprovalRunIdRef.current = "";
      return;
    }
    if (!canQuery || !teamIdTrimmed) return;
    if (approvalsBusy || approvals !== null) return;
    if (lastAutoApprovalRunIdRef.current === approvalRunIdTrimmed) return;
    lastAutoApprovalRunIdRef.current = approvalRunIdTrimmed;
    void handleApprovalsRefresh();
  }, [approvalRunIdTrimmed, approvals, approvalsBusy, canQuery, handleApprovalsRefresh, teamIdTrimmed]);

  return {
    approvalsLastSyncMs,
    approvalRunId,
    setApprovalRunId,
    approvalsBusy,
    approvalMemberId,
    setApprovalMemberId,
    approvalDecision,
    setApprovalDecision,
    approvalRuleId,
    setApprovalRuleId,
    approvalReason,
    setApprovalReason,
    approvalsError,
    approvals,
    handleApprovalsRefresh,
    handleApprovalSubmit,
  };
}
