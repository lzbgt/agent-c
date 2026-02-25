import React from "react";
import FieldLabel from "../FieldLabel";

type TeamRunRecentRunsPanelProps = {
  canQuery: boolean;
  teamId: string;
  recentRunsLimit: number;
  setRecentRunsLimit: (value: number) => void;
  recentRunsStatus: string;
  setRecentRunsStatus: (value: string) => void;
  recentRunsLive: boolean;
  setRecentRunsLive: (value: boolean) => void;
  recentRunsBusy: boolean;
  onRefresh: () => void;
  recentRunsError: string | null;
  recentRunsItems: any[];
  fmtTs: (ms?: number | null) => string;
  fmtSummary: (summary?: any) => string;
  onLoadRun: (runId: string) => void;
};

export default function TeamRunRecentRunsPanel(props: TeamRunRecentRunsPanelProps) {
  const canQuery = props.canQuery && !!props.teamId;
  return (
    <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="text-[11px] text-white/70">Recent team runs</div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Limit</FieldLabel>
          <input
            className="w-16 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={String(props.recentRunsLimit)}
            onChange={(e) => {
              const next = Number.parseInt(e.target.value, 10);
              if (Number.isFinite(next) && next > 0) {
                props.setRecentRunsLimit(next);
              } else if (!e.target.value.trim()) {
                props.setRecentRunsLimit(10);
              }
            }}
            placeholder="10"
          />
          <FieldLabel>Status</FieldLabel>
          <input
            className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.recentRunsStatus}
            onChange={(e) => props.setRecentRunsStatus(e.target.value)}
            placeholder="optional"
          />
          <label className="flex items-center gap-2 text-[10px] text-white/60">
            <input
              type="checkbox"
              className="rounded border-white/20 bg-black/40"
              checked={props.recentRunsLive}
              onChange={(e) => props.setRecentRunsLive(e.target.checked)}
            />
            live (SSE)
          </label>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || props.recentRunsBusy}
            onClick={() => props.onRefresh()}
          >
            {props.recentRunsBusy ? "Loading…" : "Refresh"}
          </button>
        </div>
      </div>
      {props.recentRunsError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {props.recentRunsError}
        </div>
      ) : null}
      {props.recentRunsItems.length > 0 ? (
        <div className="grid gap-1 text-[11px] text-white/70">
          {props.recentRunsItems.map((row: any, idx: number) => {
            const runId = String(row?.team_run_id || "");
            return (
              <div
                key={`team-run-row-${runId || idx}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
              >
                <div className="flex flex-wrap items-center gap-2">
                  <span className="text-white/90">{runId || "?"}</span>
                  {row?.status ? <span>· {row.status}</span> : null}
                  {row?.mode ? <span>· {row.mode}</span> : null}
                  {typeof row?.created_unix_ms === "number" ? <span>· {props.fmtTs(row.created_unix_ms)}</span> : null}
                  {props.fmtSummary(row?.member_job_summary) ? <span>· {props.fmtSummary(row.member_job_summary)}</span> : null}
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => {
                    if (!runId) return;
                    props.onLoadRun(runId);
                  }}
                >
                  Load
                </button>
              </div>
            );
          })}
        </div>
      ) : (
        <div className="text-[11px] text-white/50">No recent runs.</div>
      )}
    </div>
  );
}
