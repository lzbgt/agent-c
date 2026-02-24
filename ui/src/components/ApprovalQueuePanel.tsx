import React from "react";
import {
  apiGetApproval,
  apiListApprovals,
  apiPostApprovalDecision,
  type ApiAuth,
  type ApprovalDecision,
  type ApprovalRequest,
} from "../api";
import FieldLabel from "./FieldLabel";

export type ApprovalQueuePanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

export default function ApprovalQueuePanel(props: ApprovalQueuePanelProps) {
  const base = String(props.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;

  const [statusFilter, setStatusFilter] = React.useState<string>("");
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

  const parseLimit = (raw: string, fallback = 100) => {
    const n = Number.parseInt(String(raw || "").trim(), 10);
    if (!Number.isFinite(n) || n <= 0) return fallback;
    return Math.min(n, 500);
  };

  const loadApprovals = async () => {
    setListError(null);
    setListBusy(true);
    try {
      if (!canQuery) throw new Error("missing base URL");
      const res = await apiListApprovals(
        base,
        {
          status: statusFilter.trim() || undefined,
          traceId: traceIdFilter.trim() || undefined,
          jobId: jobIdFilter.trim() || undefined,
          toolName: toolNameFilter.trim() || undefined,
          runId: runIdFilter.trim() || undefined,
          limit: parseLimit(limitFilter),
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "failed to load approvals");
      setApprovals(res.approvals ?? []);
    } catch (err) {
      setListError(String(err));
    } finally {
      setListBusy(false);
    }
  };

  const clearApprovals = () => {
    setApprovals([]);
    setListError(null);
  };

  const loadDetail = async (approvalId: string) => {
    setDetailError(null);
    setDetailBusy(true);
    try {
      const res = await apiGetApproval(base, approvalId, props.auth);
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
  };

  const selectApproval = (approvalId: string) => {
    setSelectedId(approvalId);
    setDecisionError(null);
    setMemberId("");
    setMemberRole("");
    setNote("");
    void loadDetail(approvalId);
  };

  const submitDecision = async (decision: "approve" | "deny") => {
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
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "decision failed");
      await loadDetail(selectedId);
      await loadApprovals();
    } catch (err) {
      setDecisionError(String(err));
    } finally {
      setDecisionBusy(false);
    }
  };

  const selectedStatus = detailApproval?.status || "";
  const isPending = selectedStatus === "pending" || !selectedStatus;

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Approval queue</div>
          <div className="text-[11px] text-white/50">Gate tool calls with manual approvals</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="grid gap-3 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Status</FieldLabel>
              <select
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={statusFilter}
                onChange={(e) => setStatusFilter(e.target.value)}
              >
                <option value="">all</option>
                <option value="pending">pending</option>
                <option value="approved">approved</option>
                <option value="denied">denied</option>
                <option value="expired">expired</option>
              </select>
            </div>
            <div className="grid gap-1">
              <FieldLabel>Trace ID</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="trace_..."
                value={traceIdFilter}
                onChange={(e) => setTraceIdFilter(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Job ID</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="job_..."
                value={jobIdFilter}
                onChange={(e) => setJobIdFilter(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Tool name</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="shell_exec"
                value={toolNameFilter}
                onChange={(e) => setToolNameFilter(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Run ID</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="123"
                value={runIdFilter}
                onChange={(e) => setRunIdFilter(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Limit</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="100"
                value={limitFilter}
                onChange={(e) => setLimitFilter(e.target.value)}
              />
            </div>
          </div>
          <div className="mt-3 flex flex-wrap gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={listBusy || !canQuery}
              onClick={loadApprovals}
            >
              {listBusy ? "Loading…" : "Load approvals"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={listBusy}
              onClick={clearApprovals}
            >
              Clear
            </button>
          </div>
          {listError ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
              {listError}
            </div>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="flex items-center justify-between">
            <div className="text-xs font-semibold text-white/80">Approvals</div>
            <div className="text-[11px] text-white/50">{approvals.length} items</div>
          </div>
          {approvals.length === 0 ? (
            <div className="mt-2 text-xs text-white/40">No approvals loaded.</div>
          ) : (
            <div className="mt-2 grid gap-2">
              {approvals.map((a) => {
                const isSelected = selectedId === a.approval_id;
                return (
                  <div
                    key={a.approval_id || Math.random().toString(16)}
                    className={`rounded-md border px-3 py-2 text-xs text-white/80 ${
                      isSelected ? "border-emerald-400/60 bg-emerald-500/10" : "border-white/10 bg-black/30"
                    }`}
                  >
                    <div className="flex flex-wrap items-center justify-between gap-2">
                      <div className="font-semibold text-white/90">{a.approval_id || "(missing id)"}</div>
                      <div className="text-[11px] text-white/50">{a.status || "pending"}</div>
                    </div>
                    <div className="mt-1 grid gap-1 text-[11px] text-white/60">
                      <div>Tool: {a.tool_name || "-"}</div>
                      <div>Trace: {a.trace_id || "-"}</div>
                      <div>Run: {a.run_id ?? "-"}</div>
                      <div>Created: {a.created_unix_ms ?? "-"}</div>
                    </div>
                    <div className="mt-2 flex flex-wrap gap-2">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        disabled={!a.approval_id}
                        onClick={() => a.approval_id && selectApproval(a.approval_id)}
                      >
                        {isSelected ? "Reload" : "Details"}
                      </button>
                    </div>

                    {isSelected ? (
                      <div className="mt-3 rounded-md border border-white/10 bg-black/20 p-2">
                        {detailBusy ? (
                          <div className="text-[11px] text-white/60">Loading details…</div>
                        ) : detailError ? (
                          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                            {detailError}
                          </div>
                        ) : (
                          <div className="grid gap-2">
                            <div className="text-[11px] text-white/70">
                              Required approvals: {detailApproval?.required_approvals ?? "-"}
                            </div>
                            {detailApproval?.role_constraints && detailApproval.role_constraints.length > 0 ? (
                              <div className="text-[11px] text-white/70">
                                Role constraints: {detailApproval.role_constraints.join(", ")}
                              </div>
                            ) : null}
                            <div className="text-[11px] text-white/70">
                              Decision reason: {detailApproval?.decision_reason || "-"}
                            </div>
                            <div className="text-[11px] text-white/70">
                              Decisions: {detailDecisions.length}
                            </div>
                            {detailDecisions.length > 0 ? (
                              <div className="grid gap-1">
                                {detailDecisions.map((d) => (
                                  <div
                                    key={`${d.id ?? ""}-${d.member_id ?? ""}-${d.decision ?? ""}`}
                                    className="rounded border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                                  >
                                    {d.member_id || "?"}
                                    {d.member_role ? ` (${d.member_role})` : ""}: {d.decision || "?"} ({d.decision_unix_ms ?? "-"})
                                  </div>
                                ))}
                              </div>
                            ) : null}

                            <div className="grid gap-2">
                              <FieldLabel>Submit decision</FieldLabel>
                              <input
                                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                                placeholder="member_id"
                                value={memberId}
                                onChange={(e) => setMemberId(e.target.value)}
                              />
                              <input
                                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                                placeholder="member_role (optional)"
                                value={memberRole}
                                onChange={(e) => setMemberRole(e.target.value)}
                                list="approval-role-constraints"
                              />
                              <datalist id="approval-role-constraints">
                                {(detailApproval?.role_constraints ?? []).map((r, idx) => (
                                  <option key={`role-${idx}-${r}`} value={r} />
                                ))}
                              </datalist>
                              <input
                                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                                placeholder="note (optional)"
                                value={note}
                                onChange={(e) => setNote(e.target.value)}
                              />
                              <div className="flex flex-wrap gap-2">
                                <button
                                  className="rounded-md border border-emerald-500/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
                                  type="button"
                                  disabled={!isPending || decisionBusy}
                                  onClick={() => submitDecision("approve")}
                                >
                                  Approve
                                </button>
                                <button
                                  className="rounded-md border border-rose-500/40 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-500/20 disabled:opacity-50"
                                  type="button"
                                  disabled={!isPending || decisionBusy}
                                  onClick={() => submitDecision("deny")}
                                >
                                  Deny
                                </button>
                              </div>
                              {decisionError ? (
                                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                                  {decisionError}
                                </div>
                              ) : null}
                            </div>
                          </div>
                        )}
                      </div>
                    ) : null}
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
