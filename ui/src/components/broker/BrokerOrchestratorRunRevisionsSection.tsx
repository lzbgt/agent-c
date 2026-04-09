import React from "react";
import { fmtTs } from "./teamRunUtils";
import { diffKeyLabel, diffSummary, formatJson, revisionVersion, toNumber } from "./brokerOrchestratorRunUtils";
import type { BrokerOrchestratorRunState } from "./useBrokerOrchestratorRunState";

type BrokerOrchestratorRunRevisionsSectionProps = {
  state: BrokerOrchestratorRunState;
};

export default function BrokerOrchestratorRunRevisionsSection(props: BrokerOrchestratorRunRevisionsSectionProps) {
  const { state } = props;

  return (
    <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
      <div className="text-[11px] text-white/60">Revision history</div>
      <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <span>Filter</span>
        <input
          className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/90"
          placeholder="version / updated_by / goal / diff key"
          value={state.revisionFilter}
          onChange={(e) => state.setRevisionFilter(e.target.value)}
        />
        <span className="text-[10px] text-white/40">
          {state.totalFiltered}/{state.totalRevisions}
        </span>
        <div className="flex items-center gap-1">
          {(["all", "goal", "role"] as const).map((scope) => (
            <button
              key={scope}
              className={`rounded-md border px-2 py-0.5 text-[10px] ${
                state.revisionFilterScope === scope
                  ? "border-emerald-400/40 bg-emerald-500/10 text-emerald-100"
                  : "border-white/10 bg-black/30 text-white/70 hover:bg-black/40"
              }`}
              type="button"
              onClick={() => state.setRevisionScope(scope)}
            >
              {scope}
            </button>
          ))}
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
          type="button"
          onClick={() => state.setRevisionFilter("")}
          disabled={!state.revisionFilter.trim()}
        >
          Clear
        </button>
      </div>

      <div className="grid gap-3 md:grid-cols-2">
        <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
          <div className="flex items-center justify-between gap-2">
            <div className="text-[11px] text-white/70">
              Goal revisions <span className="text-white/40">({state.filteredGoalRevisions.length}/{state.scopedGoalRevisions.length})</span>
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => state.setShowGoalRevisions((prev) => !prev)}
            >
              {state.showGoalRevisions ? "Hide" : "Show"}
            </button>
          </div>
          {state.filteredGoalRevisions.length === 0 ? (
            <div className="text-[11px] text-white/50">{state.goalRevisionEmptyLabel}</div>
          ) : state.showGoalRevisions ? (
            <div className="grid gap-2">
              {state.filteredGoalRevisions.map((entry, idx) => {
                const version = revisionVersion(entry);
                const updated = toNumber(entry.updated_unix_ms);
                const diff = entry.goal_contract_diff || null;
                const summary = diffSummary(diff);
                const diffKeysLabel = diffKeyLabel(diff);
                const goalChanged = entry.goal_changed === true;
                const contractChanged = entry.goal_contract_changed === true;
                return (
                  <div
                    key={`goal-rev-${version}-${idx}`}
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <details className="group">
                      <summary className="cursor-pointer text-white/80">
                        v{version || "?"}
                        {updated ? ` · ${fmtTs(updated)}` : ""}
                        {entry.updated_by ? ` · ${String(entry.updated_by)}` : ""}
                      </summary>
                      <div className="mt-1 grid gap-2">
                        {entry.goal ? <div className="text-white/70">{String(entry.goal)}</div> : null}
                        {goalChanged || contractChanged ? (
                          <div className="text-white/40">
                            {goalChanged ? "goal changed" : null}
                            {goalChanged && contractChanged ? " · " : null}
                            {contractChanged ? "goal contract changed" : null}
                          </div>
                        ) : null}
                        {summary ? <div className="text-white/50">diff {summary}</div> : null}
                        {diffKeysLabel ? <div className="text-white/40">{diffKeysLabel}</div> : null}
                        <div className="flex flex-wrap items-center gap-2">
                          {entry.goal ? (
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() => void state.handleCopyJson(entry.goal, "goal")}
                            >
                              Copy goal
                            </button>
                          ) : null}
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => void state.handleCopyJson(entry.goal_contract, "goal contract")}
                          >
                            Copy contract
                          </button>
                          {diff ? (
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() => void state.handleCopyJson(diff, "goal contract diff")}
                            >
                              Copy diff
                            </button>
                          ) : null}
                        </div>
                        <pre className="max-h-52 overflow-auto rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[10px] text-white/70">
                          {formatJson(entry.goal_contract) || "// empty"}
                        </pre>
                      </div>
                    </details>
                  </div>
                );
              })}
            </div>
          ) : (
            <div className="text-[11px] text-white/50">
              Latest v{revisionVersion(state.latestGoalRevision) || "?"}
              {state.latestGoalRevision && toNumber(state.latestGoalRevision.updated_unix_ms)
                ? ` · ${fmtTs(Number(state.latestGoalRevision.updated_unix_ms))}`
                : ""}
              {state.latestGoalChangeSummary ? ` · ${state.latestGoalChangeSummary}` : ""}
            </div>
          )}
        </div>

        <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
          <div className="flex items-center justify-between gap-2">
            <div className="text-[11px] text-white/70">
              Role plan revisions <span className="text-white/40">({state.filteredRoleRevisions.length}/{state.scopedRoleRevisions.length})</span>
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => state.setShowRoleRevisions((prev) => !prev)}
            >
              {state.showRoleRevisions ? "Hide" : "Show"}
            </button>
          </div>
          {state.filteredRoleRevisions.length === 0 ? (
            <div className="text-[11px] text-white/50">{state.roleRevisionEmptyLabel}</div>
          ) : state.showRoleRevisions ? (
            <div className="grid gap-2">
              {state.filteredRoleRevisions.map((entry, idx) => {
                const version = revisionVersion(entry);
                const updated = toNumber(entry.updated_unix_ms);
                const summary = diffSummary(entry.role_plan_diff);
                const diffKeysLabel = diffKeyLabel(entry.role_plan_diff);
                return (
                  <div
                    key={`role-rev-${version}-${idx}`}
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <details className="group">
                      <summary className="cursor-pointer text-white/80">
                        v{version || "?"}
                        {updated ? ` · ${fmtTs(updated)}` : ""}
                        {entry.updated_by ? ` · ${String(entry.updated_by)}` : ""}
                      </summary>
                      <div className="mt-1 grid gap-2">
                        {summary ? <div className="text-white/50">diff {summary}</div> : null}
                        {diffKeysLabel ? <div className="text-white/40">{diffKeysLabel}</div> : null}
                        <div className="flex flex-wrap items-center gap-2">
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => void state.handleCopyJson(entry.role_plan_snapshot, "role plan snapshot")}
                          >
                            Copy snapshot
                          </button>
                          {entry.role_plan_diff ? (
                            <button
                              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                              type="button"
                              onClick={() => void state.handleCopyJson(entry.role_plan_diff, "role plan diff")}
                            >
                              Copy diff
                            </button>
                          ) : null}
                        </div>
                        <pre className="max-h-52 overflow-auto rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[10px] text-white/70">
                          {formatJson(entry.role_plan_snapshot) || "// empty"}
                        </pre>
                      </div>
                    </details>
                  </div>
                );
              })}
            </div>
          ) : (
            <div className="text-[11px] text-white/50">
              Latest v{revisionVersion(state.latestRoleRevision) || "?"}
              {state.latestRoleRevision && toNumber(state.latestRoleRevision.updated_unix_ms)
                ? ` · ${fmtTs(Number(state.latestRoleRevision.updated_unix_ms))}`
                : ""}
            </div>
          )}
        </div>
      </div>

      <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="text-[11px] text-white/70">Latest JSON</div>
          {state.copyNote ? <div className="text-[11px] text-emerald-200">{state.copyNote}</div> : null}
        </div>
        <div className="grid gap-2 md:grid-cols-2">
          <div className="grid gap-2">
            <div className="flex items-center justify-between gap-2">
              <div className="text-[11px] text-white/60">Goal contract</div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => state.setShowGoalContractJson((prev) => !prev)}
                >
                  {state.showGoalContractJson ? "Hide" : "Show"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => void state.handleCopyJson(state.latestGoalContract, "goal contract")}
                >
                  Copy
                </button>
              </div>
            </div>
            {state.showGoalContractJson ? (
              <pre className="max-h-60 overflow-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70">
                {formatJson(state.latestGoalContract) || "// empty"}
              </pre>
            ) : (
              <div className="text-[11px] text-white/50">{state.latestGoalContract ? "Available" : "Empty"}</div>
            )}
          </div>

          <div className="grid gap-2">
            <div className="flex items-center justify-between gap-2">
              <div className="text-[11px] text-white/60">Role plan snapshot</div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => state.setShowRolePlanJson((prev) => !prev)}
                >
                  {state.showRolePlanJson ? "Hide" : "Show"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => void state.handleCopyJson(state.latestRolePlanSnapshot, "role plan")}
                >
                  Copy
                </button>
              </div>
            </div>
            {state.showRolePlanJson ? (
              <pre className="max-h-60 overflow-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70">
                {formatJson(state.latestRolePlanSnapshot) || "// empty"}
              </pre>
            ) : (
              <div className="text-[11px] text-white/50">{state.latestRolePlanSnapshot ? "Available" : "Empty"}</div>
            )}
          </div>
        </div>
      </div>

      <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
        <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
          <div>Recent revision events</div>
          {state.revisionEventsOverflow ? (
            <div className="text-[10px] text-white/40">
              Showing {state.revisionEventsDisplay.length} of {state.revisionEventsTotal}
            </div>
          ) : null}
        </div>
        {state.revisionEventsTotal === 0 ? (
          <div className="text-[11px] text-white/50">{state.revisionEventsEmptyLabel}</div>
        ) : (
          <div className="grid gap-2">
            {state.revisionEventsDisplay.map((row, idx) => {
              const event = row.row;
              const payload = row.payload;
              const type = row.type;
              const isGoal = type === "orchestrator_goal_revision";
              const label = isGoal ? "Goal revision" : "Role plan revision";
              const version = payload.version || 0;
              const diff = isGoal ? payload.goal_contract_diff : payload.role_plan_diff;
              const summary = diffSummary(diff);
              const diffKeysLabel = diffKeyLabel(diff);
              const updatedBy = payload.updated_by ? String(payload.updated_by) : "";
              const goalChanged = isGoal && payload.goal_changed === true;
              const contractChanged = isGoal && payload.goal_contract_changed === true;
              const goalText = isGoal && payload.goal ? String(payload.goal) : "";
              return (
                <div
                  key={`${type}-${event.event_id || event.ts_unix_ms || idx}`}
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-white/80">
                    {label} · v{version || "?"}
                    {event.ts_unix_ms ? ` · ${fmtTs(event.ts_unix_ms)}` : ""}
                  </div>
                  {updatedBy ? <div className="text-white/50">by {updatedBy}</div> : null}
                  {goalChanged || contractChanged ? (
                    <div className="text-white/40">
                      {goalChanged ? "goal changed" : null}
                      {goalChanged && contractChanged ? " · " : null}
                      {contractChanged ? "goal contract changed" : null}
                    </div>
                  ) : null}
                  {goalText ? <div className="text-white/60">{goalText}</div> : null}
                  {summary ? <div className="text-white/50">diff {summary}</div> : null}
                  {diffKeysLabel ? <div className="text-white/40">{diffKeysLabel}</div> : null}
                  {diff ? (
                    <button
                      className="mt-1 rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => void state.handleCopyJson(diff, `${isGoal ? "goal" : "role plan"} diff`)}
                    >
                      Copy diff
                    </button>
                  ) : null}
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
