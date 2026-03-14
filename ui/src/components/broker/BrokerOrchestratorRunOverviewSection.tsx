import React from "react";
import FieldLabel from "../FieldLabel";
import { fmtTs } from "./teamRunUtils";
import { fmtAge, toNumber } from "./brokerOrchestratorRunUtils";
import type { BrokerOrchestratorRunState } from "./useBrokerOrchestratorRunState";

type BrokerOrchestratorRunOverviewSectionProps = {
  canQuery: boolean;
  state: BrokerOrchestratorRunState;
};

export default function BrokerOrchestratorRunOverviewSection(props: BrokerOrchestratorRunOverviewSectionProps) {
  const { canQuery, state } = props;

  return (
    <>
      <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
        <div className="text-[11px] text-white/60">Create orchestrator run</div>
        <div className="grid gap-1">
          <FieldLabel>Goal</FieldLabel>
          <textarea
            data-testid="orchestrator-run-create-goal"
            className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
            value={state.createGoal}
            onChange={(e) => state.setCreateGoal(e.target.value)}
          />
        </div>
        <div className="grid gap-2 md:grid-cols-3">
          <div className="grid gap-1">
            <FieldLabel>Status</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.createStatus}
              onChange={(e) => state.setCreateStatus(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Role plan snapshot (JSON, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              placeholder={state.rolePlanDefaults === "none" ? "{}" : "// default from team meta"}
              value={state.createRolePlanJson}
              onChange={(e) => state.setCreateRolePlanJson(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Goal contract (JSON, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              placeholder='{"success_criteria":["..."]}'
              value={state.createGoalContractJson}
              onChange={(e) => state.setCreateGoalContractJson(e.target.value)}
            />
          </div>
        </div>
        <div className="grid gap-1">
          <FieldLabel>Meta (JSON, optional)</FieldLabel>
          <textarea
            className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
            value={state.createMetaJson}
            onChange={(e) => state.setCreateMetaJson(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <button
            data-testid="orchestrator-run-create"
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || state.createBusy}
            onClick={() => void state.handleCreate()}
          >
            {state.createBusy ? "Creating…" : "Create run"}
          </button>
          {state.createNote ? <div className="text-[11px] text-emerald-200">{state.createNote}</div> : null}
        </div>
        {state.createError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.createError}
          </div>
        ) : null}
      </div>

      <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="text-[11px] text-white/60">Recent runs</div>
          <div className="flex items-center gap-2">
            <input
              className="w-32 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/90"
              placeholder="status filter"
              value={state.statusFilter}
              onChange={(e) => state.setStatusFilter(e.target.value)}
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!canQuery || state.listBusy}
              onClick={() => void state.loadRuns()}
            >
              {state.listBusy ? "Loading…" : "Refresh"}
            </button>
          </div>
        </div>
        {state.listError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.listError}
          </div>
        ) : null}
        {state.runs.length === 0 ? (
          <div className="text-[11px] text-white/50">No orchestrator runs.</div>
        ) : (
          <div className="grid gap-2">
            {state.runs.map((run) => {
              const rid = String(run?.orchestrator_run_id || "");
              const listMeta = run?.meta && typeof run.meta === "object" ? run.meta : null;
              const listGoalVersion = toNumber(listMeta?.goal_version);
              const listRoleVersion = toNumber(listMeta?.role_plan_version);
              const versionParts: string[] = [];
              if (listGoalVersion) versionParts.push(`goal v${listGoalVersion}`);
              if (listRoleVersion) versionParts.push(`role v${listRoleVersion}`);
              return (
                <button
                  key={rid}
                  data-testid={`orchestrator-run-row-${rid}`}
                  className={`flex flex-wrap items-center justify-between gap-2 rounded-md border px-2 py-1 text-[11px] ${
                    rid === state.runId
                      ? "border-emerald-400/40 bg-emerald-500/10 text-emerald-100"
                      : "border-white/10 bg-black/40 text-white/70 hover:bg-black/30"
                  }`}
                  type="button"
                  onClick={() => {
                    state.setRunId(rid);
                    void state.loadRun(rid);
                  }}
                >
                  <span>{rid || "run"}</span>
                  <span className="text-[11px] text-white/60">
                    {run?.status ? run.status : "unknown"}
                    {run?.created_unix_ms ? ` · ${fmtTs(run.created_unix_ms)}` : ""}
                    {versionParts.length > 0 ? ` · ${versionParts.join(" · ")}` : ""}
                  </span>
                </button>
              );
            })}
          </div>
        )}
      </div>

      <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="text-[11px] text-white/60">Run details</div>
          <button
            data-testid="orchestrator-run-load"
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            disabled={!canQuery || state.runBusy || !state.runId}
            onClick={() => void state.loadRun()}
          >
            {state.runBusy ? "Loading…" : "Load"}
          </button>
        </div>
        <div className="grid gap-1">
          <FieldLabel>Run id</FieldLabel>
          <input
            data-testid="orchestrator-run-id"
            className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
            value={state.runId}
            onChange={(e) => state.setRunId(e.target.value)}
          />
        </div>
        {state.runError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.runError}
          </div>
        ) : null}
        {state.currentRun ? (
          <div className="rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/70">
            <div className="text-white/80">{state.currentRun.goal || "goal"}</div>
            <div className="text-[11px] text-white/50">
              {state.currentRun.status || "status"}
              {state.currentRun.updated_unix_ms ? ` · updated ${fmtTs(state.currentRun.updated_unix_ms)}` : ""}
              {state.currentRun.last_heartbeat_unix_ms ? ` · heartbeat ${fmtTs(state.currentRun.last_heartbeat_unix_ms)}` : ""}
              {state.currentRun.lease_status ? ` · lease ${state.currentRun.lease_status}` : ""}
              {state.currentRun.heartbeat_age_ms !== undefined && state.currentRun.heartbeat_age_ms !== null
                ? ` · hb age ${fmtAge(Number(state.currentRun.heartbeat_age_ms))}`
                : ""}
              {state.currentRun.lease_timeout_ms !== undefined && state.currentRun.lease_timeout_ms !== null
                ? ` · lease timeout ${fmtAge(Number(state.currentRun.lease_timeout_ms))}`
                : ""}
            </div>
            <div className="text-[11px] text-white/50">
              {state.currentOwner ? `owner ${state.currentOwner}` : "owner unclaimed"}
              {state.prevOwner ? ` · prev ${state.prevOwner}` : ""}
              {state.allowTakeover ? ` · allow_takeover ${state.allowTakeover}` : ""}
            </div>
            {state.revisionSummary ? <div className="text-[11px] text-white/50">{state.revisionSummary}</div> : null}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No run loaded.</div>
        )}
      </div>
    </>
  );
}
