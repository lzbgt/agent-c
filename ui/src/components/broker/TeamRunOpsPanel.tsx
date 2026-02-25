import React from "react";
import FieldLabel from "../FieldLabel";
import type { BrokerEventRow, TeamMemberRow, TeamQuorumRuleRow } from "./types";

type TeamRunOpsPanelProps = {
  canQuery: boolean;
  teamId: string;
  runtimeUpdateMode: string;
  setRuntimeUpdateMode: (value: string) => void;
  runtimeUpdateBusy: boolean;
  runtimeUpdateError: string | null;
  runtimeUpdateNote: string;
  onRuntimeMembersLoadFromRun: () => void;
  onRuntimeMembersUpdate: () => void;
  approvalsLastSyncMs: number | null;
  fmtTs: (ms?: number | null) => string;
  quorumRequestRows: BrokerEventRow[];
  onTeamSelect?: (teamId: string) => void;
  setApprovalRunId: (value: string) => void;
  setRunLookupId: (value: string) => void;
  approvalRunId: string;
  approvalsBusy: boolean;
  onApprovalsRefresh: () => void;
  membersList: TeamMemberRow[];
  approvalMemberId: string;
  setApprovalMemberId: (value: string) => void;
  approvalDecision: string;
  setApprovalDecision: (value: string) => void;
  approvalRuleId: string;
  setApprovalRuleId: (value: string) => void;
  rulesList: TeamQuorumRuleRow[];
  approvalReason: string;
  setApprovalReason: (value: string) => void;
  onApprovalSubmit: () => void;
  approvalsError: string | null;
  approvals: Array<{
    approval_id?: string;
    member_id?: string;
    decision?: string;
    rule_id?: string;
    role?: string;
    created_by?: string;
    created_unix_ms?: number;
    reason?: string;
  }> | null;
};

export default function TeamRunOpsPanel(props: TeamRunOpsPanelProps) {
  const canQuery = props.canQuery && !!props.teamId;
  return (
    <>
      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Runtime member updates</div>
        <div className="text-[11px] text-white/50">
          Apply the runtime members JSON (above) to an existing run; merge preserves existing members by member_id.
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Mode</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            data-testid="team-run-runtime-mode"
            value={props.runtimeUpdateMode}
            onChange={(e) => props.setRuntimeUpdateMode(e.target.value)}
          >
            <option value="replace">replace</option>
            <option value="merge">merge</option>
          </select>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            data-testid="team-run-runtime-load"
            disabled={!props.canQuery || props.runtimeUpdateBusy}
            onClick={() => props.onRuntimeMembersLoadFromRun()}
          >
            Load from run
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            data-testid="team-run-runtime-update"
            disabled={!props.canQuery || props.runtimeUpdateBusy}
            onClick={() => props.onRuntimeMembersUpdate()}
          >
            {props.runtimeUpdateBusy ? "Updating…" : "Update runtime members"}
          </button>
        </div>
        {props.runtimeUpdateError ? (
          <div className="text-[11px] text-rose-200">{props.runtimeUpdateError}</div>
        ) : null}
        {props.runtimeUpdateNote ? <div className="text-[11px] text-white/60">{props.runtimeUpdateNote}</div> : null}
      </div>

      <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Run approvals</div>
        <div className="text-[11px] text-white/50">Submit or review approvals for a team run (quorum rules apply).</div>
        {props.approvalsLastSyncMs ? (
          <div className="text-[11px] text-white/50">Last sync: {props.fmtTs(props.approvalsLastSyncMs)}</div>
        ) : null}
        {props.quorumRequestRows.length > 0 ? (
          <div className="grid gap-2">
            <div className="text-[11px] text-white/60">Recent quorum requests</div>
            {props.quorumRequestRows.map((row, idx) => {
              const payload = row?.payload ?? {};
              const teamId = payload?.team_id ? String(payload.team_id) : "";
              const runId = payload?.team_run_id ? String(payload.team_run_id) : "";
              const ruleId = payload?.rule_id ? String(payload.rule_id) : "";
              const action = payload?.action ? String(payload.action) : "";
              const min = payload?.min_approvals;
              const ts = props.fmtTs(row?.ts_unix_ms as number | null);
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
                      if (teamId && props.onTeamSelect) props.onTeamSelect(teamId);
                      if (runId) {
                        props.setApprovalRunId(runId);
                        props.setRunLookupId(runId);
                      }
                      if (ruleId) props.setApprovalRuleId(ruleId);
                    }}
                  >
                    Use
                  </button>
                </div>
              );
            })}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No quorum requests captured yet.</div>
        )}
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Run ID</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.approvalRunId}
            onChange={(e) => props.setApprovalRunId(e.target.value)}
            placeholder="defaults to run id above"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || props.approvalsBusy}
            onClick={() => props.onApprovalsRefresh()}
          >
            {props.approvalsBusy ? "Loading…" : "Load approvals"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.approvalMemberId}
            onChange={(e) => props.setApprovalMemberId(e.target.value)}
            placeholder="member id"
            list="team-approvals-members"
          />
          <datalist id="team-approvals-members">
            {props.membersList.map((m, idx) => {
              const mid = String(m?.member_id || "");
              if (!mid) return null;
              return <option key={`member-opt-${mid}-${idx}`} value={mid} />;
            })}
          </datalist>
          <FieldLabel>Decision</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.approvalDecision}
            onChange={(e) => props.setApprovalDecision(e.target.value)}
          >
            <option value="approve">approve</option>
            <option value="deny">deny</option>
          </select>
          <FieldLabel>Rule ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.approvalRuleId}
            onChange={(e) => props.setApprovalRuleId(e.target.value)}
            placeholder="optional"
            list="team-approvals-rules"
          />
          <datalist id="team-approvals-rules">
            {props.rulesList.map((r, idx) => {
              const rid = String(r?.rule_id || "");
              if (!rid) return null;
              return <option key={`rule-opt-${rid}-${idx}`} value={rid} />;
            })}
          </datalist>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Reason</FieldLabel>
          <input
            className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.approvalReason}
            onChange={(e) => props.setApprovalReason(e.target.value)}
            placeholder="optional"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || props.approvalsBusy}
            onClick={() => props.onApprovalSubmit()}
          >
            {props.approvalsBusy ? "Submitting…" : "Submit approval"}
          </button>
        </div>
        {props.approvalsError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {props.approvalsError}
          </div>
        ) : null}
        {props.approvals && props.approvals.length > 0 ? (
          <div className="grid gap-2">
            {props.approvals.map((a, idx) => (
              <div
                key={`approval-${a?.approval_id || idx}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-[11px] text-white/70">
                  <span className="text-white/90">{String(a?.member_id || "member")}</span>
                  {a?.decision ? ` · ${a.decision}` : ""}
                  {a?.rule_id ? ` · rule ${a.rule_id}` : ""}
                  {a?.role ? ` · role ${a.role}` : ""}
                  {a?.created_by ? ` · by ${a.created_by}` : ""}
                  {a?.created_unix_ms ? ` · ${props.fmtTs(a.created_unix_ms)}` : ""}
                  {a?.reason ? ` · ${a.reason}` : ""}
                </div>
              </div>
            ))}
          </div>
        ) : props.approvals ? (
          <div className="text-[11px] text-white/50">No approvals yet.</div>
        ) : null}
      </div>
    </>
  );
}
