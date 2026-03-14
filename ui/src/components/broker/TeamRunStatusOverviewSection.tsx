import React from "react";
import RoleGraphPreview from "./RoleGraphPreview";
import type { MemberSession } from "./teamRunStatusTypes";
import type { RoleGraphEdge } from "./teamRunUtils";

type TeamRunStatusOverviewSectionProps = {
  run: any;
  fmtTs: (ms?: number | null) => string;
  fmtSummary: (summary?: any) => string;
  roleGraphEdges: RoleGraphEdge[];
  roleGraphRoles: string[];
  rolePromptMode: string;
  roleInstructionCount: number;
  sharedMemoryScope: string;
  sharedMemoryMode: string;
  autoAllocateRoles: boolean;
  autoAllocateAllocated: any[];
  autoAllocateMissing: any[];
  autoAllocateWarning: string;
  runtimeUpdateBusy: boolean;
  canQuery: boolean;
  memberSessions: MemberSession[];
  onRuntimeMemberToggle: (member: any) => Promise<void> | void;
  onRuntimeMemberRemove: (member: any) => Promise<void> | void;
};

export default function TeamRunStatusOverviewSection(props: TeamRunStatusOverviewSectionProps) {
  const { run } = props;
  return (
    <>
      <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
        status {String(run?.status || "")}
        {run?.created_unix_ms ? ` · ${props.fmtTs(run.created_unix_ms)}` : ""}
        {run?.mode ? ` · mode ${String(run.mode)}` : ""}
        {typeof run?.cancel_requested_unix_ms === "number"
          ? ` · cancel requested ${props.fmtTs(run.cancel_requested_unix_ms)}`
          : ""}
      </div>
      {run?.run_overrides_mode ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          overrides mode: {String(run.run_overrides_mode)}
        </div>
      ) : null}
      {props.sharedMemoryScope ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          shared memory: {props.sharedMemoryScope}
          {props.sharedMemoryMode ? ` (${props.sharedMemoryMode})` : ""}
        </div>
      ) : null}
      {props.autoAllocateRoles ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          auto-allocate: on
          {props.autoAllocateAllocated.length > 0 ? ` · allocated ${props.autoAllocateAllocated.join(", ")}` : ""}
          {props.autoAllocateMissing.length > 0 ? ` · missing ${props.autoAllocateMissing.join(", ")}` : ""}
          {props.autoAllocateWarning ? ` · ${props.autoAllocateWarning}` : ""}
        </div>
      ) : null}
      {props.roleGraphEdges.length > 0 || props.roleGraphRoles.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-xs font-semibold text-white/80">Role graph</div>
          <RoleGraphPreview
            roles={props.roleGraphRoles}
            edges={props.roleGraphEdges}
            title="Role graph"
            emptyLabel="No role graph recorded."
          />
          {props.roleGraphEdges.length > 0 ? (
            <div className="mt-2 grid gap-1 text-[11px] text-white/60">
              {props.roleGraphEdges.map((edge, idx) => (
                <div key={`role-graph-edge-${edge.from_role}-${edge.to_role}-${idx}`}>
                  {edge.from_role || "role"} {"->"} {edge.to_role || "role"}
                  {edge.reason ? ` · ${edge.reason}` : ""}
                </div>
              ))}
            </div>
          ) : null}
          {props.rolePromptMode || props.roleInstructionCount > 0 ? (
            <div className="mt-1 text-[11px] text-white/60">
              {props.rolePromptMode ? `prompt mode ${props.rolePromptMode}` : ""}
              {props.roleInstructionCount > 0
                ? `${props.rolePromptMode ? " · " : ""}instructions ${props.roleInstructionCount}`
                : ""}
            </div>
          ) : null}
        </div>
      ) : null}
      {run?.role_overrides_applied || run?.member_overrides_applied ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          <div className="text-[11px] text-white/80">applied overrides</div>
          <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/60">
            {JSON.stringify(
              {
                role_overrides_applied: run?.role_overrides_applied ?? null,
                member_overrides_applied: run?.member_overrides_applied ?? null,
              },
              null,
              2,
            )}
          </pre>
        </div>
      ) : null}
      {props.fmtSummary(run?.member_job_summary) ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          summary: {props.fmtSummary(run?.member_job_summary)}
        </div>
      ) : null}
      {Array.isArray(run?.dispatch_errors) && run.dispatch_errors.length > 0 ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-200">
          dispatch errors:
          {run.dispatch_errors.map((err: any, idx: number) => {
            const mid = err?.member_id ? String(err.member_id) : "";
            const aid = err?.agent_id ? String(err.agent_id) : "";
            const msg = err?.error ? String(err.error) : "dispatch error";
            return (
              <div key={`dispatch-error-${mid || aid}-${idx}`}>
                {mid ? `${mid} · ` : ""}
                {aid ? `agent ${aid}` : "agent ?"} · {msg}
              </div>
            );
          })}
        </div>
      ) : null}
      {Array.isArray(run?.cancel_results) && run.cancel_results.length > 0 ? (
        <div className="rounded-md border border-amber-400/20 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          cancel results:
          {run.cancel_results.map((row: any, idx: number) => {
            const mid = row?.member_id ? String(row.member_id) : "";
            const aid = row?.agent_id ? String(row.agent_id) : "";
            const jobId = row?.job_id ? String(row.job_id) : "";
            const ok = typeof row?.ok === "boolean" ? String(row.ok) : "";
            const http = typeof row?.http_status === "number" ? `http ${row.http_status}` : "";
            const err = row?.error ? String(row.error) : "";
            return (
              <div key={`cancel-result-${mid || aid}-${idx}`}>
                {mid ? `${mid} · ` : ""}
                {aid ? `agent ${aid}` : "agent ?"}
                {jobId ? ` · job ${jobId}` : ""}
                {ok ? ` · ok ${ok}` : ""}
                {http ? ` · ${http}` : ""}
                {err ? ` · ${err}` : ""}
              </div>
            );
          })}
        </div>
      ) : null}
      {Array.isArray(run?.member_jobs) && run.member_jobs.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member jobs:
          {run.member_jobs.map((job: any, idx: number) => {
            const mid = job?.member_id ? String(job.member_id) : "";
            const aid = job?.agent_id ? String(job.agent_id) : "";
            const jobId = job?.job_id ? String(job.job_id) : "";
            const status = job?.status ? String(job.status) : "";
            const ok = typeof job?.ok === "boolean" ? String(job.ok) : "";
            const err = job?.error ? String(job.error) : job?.dispatch_error ? String(job.dispatch_error) : "";
            const updated = typeof job?.updated_unix_ms === "number" ? props.fmtTs(job.updated_unix_ms) : "";
            return (
              <div key={`member-job-${mid || aid}-${idx}`} className="mt-1">
                <div>
                  {mid ? `${mid} · ` : ""}
                  {aid ? `agent ${aid}` : "agent ?"}
                  {jobId ? ` · job ${jobId}` : ""}
                </div>
                <div className="text-[10px] text-white/50">
                  {status ? `status ${status}` : ""}
                  {ok ? ` · ok ${ok}` : ""}
                  {updated ? ` · ${updated}` : ""}
                  {err ? ` · ${err}` : ""}
                </div>
              </div>
            );
          })}
        </div>
      ) : null}
      {props.memberSessions.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member sessions:
          {props.memberSessions.map((row, idx) => (
            <div key={`member-session-${row.memberId}-${idx}`} className="mt-1 text-[10px] text-white/60">
              {row.memberId} · {row.sessionId}
            </div>
          ))}
        </div>
      ) : null}
      {Array.isArray(run?.runtime_members) && run.runtime_members.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          runtime members:
          {run.runtime_members.map((m: any, idx: number) => {
            const agentId = m?.agent_id ? String(m.agent_id) : "";
            const role = m?.role ? String(m.role) : "";
            const mid = m?.member_id ? String(m.member_id) : "";
            const status = m?.status ? String(m.status) : "";
            const rawKey = mid || agentId || `row-${idx}`;
            const runtimeKey = rawKey.replace(/[^a-zA-Z0-9_-]/g, "_");
            return (
              <div
                key={`runtime-member-${mid || agentId}-${idx}`}
                data-testid={`team-run-runtime-row-${runtimeKey}`}
                className="flex flex-wrap items-center justify-between gap-2"
              >
                <div>
                  {mid ? `${mid} · ` : ""}
                  {agentId ? `agent ${agentId}` : "agent ?"}
                  {role ? ` · role ${role}` : ""}
                  {status ? ` · ${status}` : ""}
                </div>
                <div className="flex items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    data-testid={`team-run-runtime-toggle-${runtimeKey}`}
                    disabled={!props.canQuery || props.runtimeUpdateBusy}
                    onClick={() => void props.onRuntimeMemberToggle(m)}
                  >
                    {String(status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    data-testid={`team-run-runtime-remove-${runtimeKey}`}
                    disabled={!props.canQuery || props.runtimeUpdateBusy}
                    onClick={() => void props.onRuntimeMemberRemove(m)}
                  >
                    Remove
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      ) : null}
    </>
  );
}
