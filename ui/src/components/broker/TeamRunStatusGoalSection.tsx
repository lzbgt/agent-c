import React from "react";
import FieldLabel from "../FieldLabel";

type TeamRunStatusGoalSectionProps = {
  run: any;
  goalContract: any;
  goalEventRows: any[];
  canWrite: boolean;
  goalContractGoal: string;
  setGoalContractGoal: (value: string) => void;
  goalContractCriteria: string;
  setGoalContractCriteria: (value: string) => void;
  goalContractConstraints: string;
  setGoalContractConstraints: (value: string) => void;
  goalEventType: string;
  setGoalEventType: (value: string) => void;
  goalEventMessage: string;
  setGoalEventMessage: (value: string) => void;
  goalEventData: string;
  setGoalEventData: (value: string) => void;
  goalUpdateBusy: boolean;
  goalUpdateError: string | null;
  goalUpdateNote: string | null;
  fmtTs: (ms?: number | null) => string;
  onGoalContractUpdate: () => Promise<void> | void;
  onGoalEvent: () => Promise<void> | void;
};

export default function TeamRunStatusGoalSection(props: TeamRunStatusGoalSectionProps) {
  return (
    <>
      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Goal contract</div>
        {props.goalContract ? (
          <div className="text-[11px] text-white/60">
            <div>goal: {typeof props.goalContract.goal === "string" ? props.goalContract.goal : "(missing)"}</div>
            {Array.isArray(props.goalContract.success_criteria) && props.goalContract.success_criteria.length > 0 ? (
              <div>success: {props.goalContract.success_criteria.join(" · ")}</div>
            ) : null}
            {Array.isArray(props.goalContract.constraints) && props.goalContract.constraints.length > 0 ? (
              <div>constraints: {props.goalContract.constraints.join(" · ")}</div>
            ) : null}
            {typeof props.run?.goal_updated_unix_ms === "number" ? (
              <div>updated: {props.fmtTs(props.run.goal_updated_unix_ms)}</div>
            ) : null}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No goal contract recorded.</div>
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Goal</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.goalContractGoal}
              onChange={(e) => props.setGoalContractGoal(e.target.value)}
              placeholder="Primary run goal"
              disabled={!props.canWrite || props.goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Success criteria (one per line)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={props.goalContractCriteria}
              onChange={(e) => props.setGoalContractCriteria(e.target.value)}
              disabled={!props.canWrite || props.goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Constraints (one per line)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={props.goalContractConstraints}
              onChange={(e) => props.setGoalContractConstraints(e.target.value)}
              disabled={!props.canWrite || props.goalUpdateBusy}
            />
          </div>
          <button
            className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!props.canWrite || props.goalUpdateBusy}
            onClick={() => void props.onGoalContractUpdate()}
          >
            {props.goalUpdateBusy ? "Updating..." : "Apply goal contract"}
          </button>
          {props.goalUpdateError ? <div className="text-[11px] text-rose-200">{props.goalUpdateError}</div> : null}
          {props.goalUpdateNote ? <div className="text-[11px] text-white/60">{props.goalUpdateNote}</div> : null}
        </div>
      </div>

      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Goal events (progress / drift / spawn_validation)</div>
        {props.goalEventRows.length > 0 ? (
          <div className="grid gap-1 text-[11px] text-white/60">
            {props.goalEventRows.map((ev: any, idx: number) => {
              const evType = ev?.type ? String(ev.type) : "event";
              const ts = typeof ev?.ts_unix_ms === "number" ? props.fmtTs(ev.ts_unix_ms) : "";
              const msg = ev?.message ? String(ev.message) : "";
              return (
                <div key={`goal-event-${ev?.event_index || idx}`} className="rounded-md border border-white/5 bg-black/20 px-2 py-1">
                  <div className="text-[11px] text-white/70">
                    {evType}
                    {ts ? ` · ${ts}` : ""}
                    {ev?.event_index ? ` · #${ev.event_index}` : ""}
                  </div>
                  {msg ? <div className="text-[11px] text-white/60">{msg}</div> : null}
                  {ev?.data ? (
                    <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/50">
                      {JSON.stringify(ev.data, null, 2)}
                    </pre>
                  ) : null}
                </div>
              );
            })}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No goal events yet.</div>
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Type</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.goalEventType}
              onChange={(e) => props.setGoalEventType(e.target.value)}
              disabled={!props.canWrite || props.goalUpdateBusy}
            >
              <option value="progress">progress</option>
              <option value="drift">drift</option>
              <option value="spawn_validation">spawn_validation</option>
            </select>
            <FieldLabel>Message</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.goalEventMessage}
              onChange={(e) => props.setGoalEventMessage(e.target.value)}
              placeholder="checkpoint or drift note"
              disabled={!props.canWrite || props.goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Data (JSON object, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={props.goalEventData}
              onChange={(e) => props.setGoalEventData(e.target.value)}
              placeholder='{"evidence":"artifact:..."}'
              disabled={!props.canWrite || props.goalUpdateBusy}
            />
          </div>
          <button
            className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!props.canWrite || props.goalUpdateBusy}
            onClick={() => void props.onGoalEvent()}
          >
            {props.goalUpdateBusy ? "Sending..." : "Emit goal event"}
          </button>
        </div>
      </div>
    </>
  );
}
