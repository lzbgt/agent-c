import React from "react";
import { apiBrokerTeamRunApprovalsCreate, apiBrokerTeamRunApprovalsList, type ApiAuth } from "../../api";
import FieldLabel from "../FieldLabel";
import type { BrokerEventRow } from "./types";

const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export type BrokerTeamApprovalsProps = {
  base: string;
  auth: ApiAuth;
  quorumEvents: BrokerEventRow[];
};

export default function BrokerTeamApprovals(props: BrokerTeamApprovalsProps) {
  const quorumRequestRows = React.useMemo(
    () => props.quorumEvents.filter((ev) => ev?.type === "team_quorum_request"),
    [props.quorumEvents],
  );

  const [approvalTeamId, setApprovalTeamId] = React.useState<string>("");
  const [approvalTeamRunId, setApprovalTeamRunId] = React.useState<string>("");
  const [approvalMemberId, setApprovalMemberId] = React.useState<string>("");
  const [approvalRuleId, setApprovalRuleId] = React.useState<string>("");
  const [approvalDecision, setApprovalDecision] = React.useState<string>("approve");
  const [approvalReason, setApprovalReason] = React.useState<string>("");
  const [approvalBusy, setApprovalBusy] = React.useState<boolean>(false);
  const [approvalError, setApprovalError] = React.useState<string | null>(null);
  const [approvalResults, setApprovalResults] = React.useState<any[] | null>(null);
  const [approvalListBusy, setApprovalListBusy] = React.useState<boolean>(false);
  const [approvalListError, setApprovalListError] = React.useState<string | null>(null);
  const [approvalListResults, setApprovalListResults] = React.useState<any[] | null>(null);

  const approvalRows = approvalResults ?? approvalListResults;

  const handleApprovalList = async () => {
    setApprovalListError(null);
    setApprovalListResults(null);
    const teamId = String(approvalTeamId || "").trim();
    const runId = String(approvalTeamRunId || "").trim();
    if (!teamId) {
      setApprovalListError("missing team_id");
      return;
    }
    if (!runId) {
      setApprovalListError("missing team_run_id");
      return;
    }
    setApprovalListBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsList(props.base, teamId, runId, props.auth);
      if (resp.status >= 400) {
        const err = resp.data?.error || resp.data?.err;
        throw new Error(err ? String(err) : `broker error (${resp.status})`);
      }
      const approvals = Array.isArray(resp.data?.approvals) ? resp.data.approvals : [];
      setApprovalListResults(approvals);
    } catch (e) {
      setApprovalListError(String(e));
    } finally {
      setApprovalListBusy(false);
    }
  };

  const handleApprovalSubmit = async () => {
    setApprovalError(null);
    setApprovalResults(null);
    const teamId = String(approvalTeamId || "").trim();
    const runId = String(approvalTeamRunId || "").trim();
    const memberId = String(approvalMemberId || "").trim();
    const ruleId = String(approvalRuleId || "").trim();
    const decision = String(approvalDecision || "").trim();
    const reason = String(approvalReason || "").trim();
    if (!teamId) {
      setApprovalError("missing team_id");
      return;
    }
    if (!runId) {
      setApprovalError("missing team_run_id");
      return;
    }
    if (!memberId) {
      setApprovalError("missing member_id");
      return;
    }
    const entry: Record<string, any> = { member_id: memberId, decision };
    if (ruleId) entry.rule_id = ruleId;
    if (reason) entry.reason = reason;
    setApprovalBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsCreate(props.base, teamId, runId, { approvals: [entry] }, props.auth);
      if (resp.status >= 400) {
        const err = resp.data?.error || resp.data?.err;
        throw new Error(err ? String(err) : `broker error (${resp.status})`);
      }
      const approvals = Array.isArray(resp.data?.approvals) ? resp.data.approvals : [];
      setApprovalResults(approvals);
    } catch (e) {
      setApprovalError(String(e));
    } finally {
      setApprovalBusy(false);
    }
  };

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 text-xs font-semibold text-white/80">Team quorum approvals</div>
      <div className="mb-2 text-[11px] text-white/50">
        Owner/admin only. Use recent quorum requests to prefill team + run identifiers.
      </div>
      {quorumRequestRows.length === 0 ? (
        <div className="mb-3 text-[11px] text-white/50">No quorum requests captured yet.</div>
      ) : (
        <div className="mb-3 grid gap-2">
          {quorumRequestRows.map((row, idx) => {
            const payload = row?.payload ?? {};
            const teamId = payload?.team_id ? String(payload.team_id) : "";
            const runId = payload?.team_run_id ? String(payload.team_run_id) : "";
            const ruleId = payload?.rule_id ? String(payload.rule_id) : "";
            const action = payload?.action ? String(payload.action) : "";
            const min = payload?.min_approvals;
            const ts = fmtTs(row?.ts_unix_ms);
            return (
              <div
                key={`quorum-request-${teamId}-${runId}-${ruleId}-${idx}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-[11px] text-white/70">
                  <span className="text-white/90">{action || "team_run"}</span>
                  {min !== undefined ? ` · min ${min}` : ""}
                  {ruleId ? ` · rule ${ruleId}` : ""}
                  {ts ? ` · ${ts}` : ""}
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => {
                    setApprovalTeamId(teamId);
                    setApprovalTeamRunId(runId);
                    setApprovalRuleId(ruleId);
                  }}
                >
                  Use
                </button>
              </div>
            );
          })}
        </div>
      )}

      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Team ID</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalTeamId}
            onChange={(e) => setApprovalTeamId(e.target.value)}
          />
          <FieldLabel>Run ID</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalTeamRunId}
            onChange={(e) => setApprovalTeamRunId(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalMemberId}
            onChange={(e) => setApprovalMemberId(e.target.value)}
          />
          <FieldLabel>Decision</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalDecision}
            onChange={(e) => setApprovalDecision(e.target.value)}
          >
            <option value="approve">approve</option>
            <option value="deny">deny</option>
          </select>
          <FieldLabel>Rule ID</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalRuleId}
            onChange={(e) => setApprovalRuleId(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Reason</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={approvalReason}
            onChange={(e) => setApprovalReason(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={approvalBusy || approvalListBusy}
            onClick={() => void handleApprovalSubmit()}
          >
            {approvalBusy ? "Submitting…" : "Submit approval"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={approvalBusy || approvalListBusy}
            onClick={() => void handleApprovalList()}
          >
            {approvalListBusy ? "Loading…" : "List approvals"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => {
              setApprovalError(null);
              setApprovalResults(null);
              setApprovalListError(null);
              setApprovalListResults(null);
            }}
          >
            Clear
          </button>
        </div>
        {approvalError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {approvalError}
          </div>
        ) : null}
        {approvalListError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {approvalListError}
          </div>
        ) : null}
        {approvalRows && approvalRows.length > 0 ? (
          <div className="grid gap-2">
            {approvalRows.map((row: any, idx: number) => {
              const decision = String(row?.decision || "");
              const memberId = String(row?.member_id || "");
              const ruleId = String(row?.rule_id || "");
              const role = String(row?.role || "");
              const created = fmtTs(row?.created_unix_ms);
              return (
                <div
                  key={`approval-${memberId}-${ruleId}-${idx}`}
                  className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-xs text-white/90">
                    {decision || "approve"} · member {memberId || "unknown"}
                  </div>
                  <div className="text-[11px] text-white/50">
                    {ruleId ? `rule ${ruleId}` : "any rule"}
                    {role ? ` · role ${role}` : ""}
                    {created ? ` · ${created}` : ""}
                  </div>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>
    </section>
  );
}
