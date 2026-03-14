import React from "react";
import FieldLabel from "../FieldLabel";
import type { BrokerOrchestratorRunState } from "./useBrokerOrchestratorRunState";

type BrokerOrchestratorRunMutationSectionProps = {
  canQuery: boolean;
  state: BrokerOrchestratorRunState;
};

export default function BrokerOrchestratorRunMutationSection(props: BrokerOrchestratorRunMutationSectionProps) {
  const { canQuery, state } = props;

  return (
    <>
      <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
        <div className="text-[11px] text-white/60">Update run</div>
        <div className="grid gap-2 md:grid-cols-3">
          <div className="grid gap-1">
            <FieldLabel>Status</FieldLabel>
            <input
              data-testid="orchestrator-run-update-status"
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateStatus}
              onChange={(e) => state.setUpdateStatus(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Goal</FieldLabel>
            <input
              data-testid="orchestrator-run-update-goal"
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateGoal}
              onChange={(e) => state.setUpdateGoal(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Meta (JSON)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateMetaJson}
              onChange={(e) => state.setUpdateMetaJson(e.target.value)}
            />
          </div>
        </div>
        <div className="grid gap-2 md:grid-cols-2">
          <div className="grid gap-1">
            <FieldLabel>Goal contract (JSON)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateGoalContractJson}
              onChange={(e) => state.setUpdateGoalContractJson(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Role plan snapshot (JSON)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateRolePlanJson}
              onChange={(e) => state.setUpdateRolePlanJson(e.target.value)}
            />
          </div>
        </div>
        <div className="grid gap-2 md:grid-cols-3">
          <div className="grid gap-1">
            <FieldLabel>Expected owner</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateExpectedOwner}
              onChange={(e) => state.setUpdateExpectedOwner(e.target.value)}
              placeholder={state.currentOwner || ""}
            />
            <label className="flex items-center gap-2 text-[11px] text-white/60">
              <input
                type="checkbox"
                checked={state.updateExpectedOwnerEmpty}
                onChange={(e) => state.setUpdateExpectedOwnerEmpty(e.target.checked)}
              />
              Expect owner empty
            </label>
          </div>
          <div className="grid gap-1">
            <FieldLabel>Expected status</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.updateExpectedStatus}
              onChange={(e) => state.setUpdateExpectedStatus(e.target.value)}
            />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <button
            data-testid="orchestrator-run-update"
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || state.updateBusy}
            onClick={() => void state.handleUpdate()}
          >
            {state.updateBusy ? "Updating…" : "Update"}
          </button>
          {state.updateNote ? <div className="text-[11px] text-emerald-200">{state.updateNote}</div> : null}
        </div>
        {state.updateError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.updateError}
          </div>
        ) : null}
      </div>

      <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
        <div className="text-[11px] text-white/60">Heartbeat</div>
        <div className="grid gap-2 md:grid-cols-3">
          <div className="grid gap-1">
            <FieldLabel>Status (optional)</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.heartbeatStatus}
              onChange={(e) => state.setHeartbeatStatus(e.target.value)}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Expected owner</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.heartbeatExpectedOwner}
              onChange={(e) => state.setHeartbeatExpectedOwner(e.target.value)}
              placeholder={state.currentOwner || ""}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Expected status</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={state.heartbeatExpectedStatus}
              onChange={(e) => state.setHeartbeatExpectedStatus(e.target.value)}
            />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <button
            data-testid="orchestrator-run-heartbeat"
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || state.heartbeatBusy}
            onClick={() => void state.handleHeartbeat()}
          >
            {state.heartbeatBusy ? "Pinging…" : "Send heartbeat"}
          </button>
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input type="checkbox" checked={state.heartbeatAuto} onChange={(e) => state.setHeartbeatAuto(e.target.checked)} />
            Auto
          </label>
          <input
            className="w-24 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
            value={state.heartbeatIntervalMs}
            onChange={(e) => state.setHeartbeatIntervalMs(Number.parseInt(e.target.value, 10) || 0)}
          />
          <span className="text-[11px] text-white/50">ms</span>
          {state.heartbeatNote ? <span className="text-[11px] text-emerald-200">{state.heartbeatNote}</span> : null}
        </div>
        {state.heartbeatError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.heartbeatError}
          </div>
        ) : null}
      </div>
    </>
  );
}
