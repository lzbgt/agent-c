import React from "react";
import { safeObject } from "../jsonUtils";

type DbRunRow = {
  run_id?: number;
  ts_unix_ms?: number;
  ok?: boolean;
  tools?: string;
  model?: string | null;
  error?: string | null;
  stop_reason?: string | null;
  steps_executed?: number | null;
  tool_calls_total?: number | null;
  last_error_reason?: string | null;
  last_error?: unknown;
};

function fmtTs(ts?: number) {
  if (typeof ts !== "number" || !Number.isFinite(ts) || ts <= 0) return "";
  try {
    return new Date(ts).toISOString();
  } catch {
    return "";
  }
}

export default function DbRunsView({
  runs,
  onSelectRunId,
}: {
  runs: DbRunRow[];
  onSelectRunId?: (runId: number) => void;
}) {
  return (
    <div className="mt-2 rounded-md border border-white/10 bg-black/30">
      <div className="flex items-center justify-between border-b border-white/10 px-3 py-2">
        <div className="text-xs text-white/70">DB runs</div>
        <div className="text-[11px] text-white/40">{runs.length} rows</div>
      </div>
      <div className="max-h-[240px] overflow-auto">
        {runs.length === 0 ? (
          <div className="px-3 py-3 text-[11px] text-white/40">(no rows)</div>
        ) : (
          <table className="w-full text-left text-[11px] text-white/70">
            <thead className="sticky top-0 bg-black/40 text-white/50">
              <tr>
                <th className="px-3 py-2">run</th>
                <th className="px-3 py-2">ts</th>
                <th className="px-3 py-2">tools</th>
                <th className="px-3 py-2">model</th>
                <th className="px-3 py-2">ok</th>
                <th className="px-3 py-2">stop</th>
                <th className="px-3 py-2">steps</th>
                <th className="px-3 py-2">tool calls</th>
                <th className="px-3 py-2">last error</th>
              </tr>
            </thead>
            <tbody>
              {runs.map((r, idx) => {
                const id = typeof r.run_id === "number" ? r.run_id : 0;
                const ok = typeof r.ok === "boolean" ? r.ok : undefined;
                const clickable = id > 0 && typeof onSelectRunId === "function";
                const stopReason = String(r.stop_reason ?? "");
                const steps = typeof r.steps_executed === "number" ? r.steps_executed : "";
                const toolCalls = typeof r.tool_calls_total === "number" ? r.tool_calls_total : "";
                const lastError = safeObject(r.last_error);
                const lastReason =
                  (r.last_error_reason && String(r.last_error_reason)) ||
                  (typeof lastError.reason === "string"
                    ? lastError.reason
                    : typeof lastError.error === "string"
                      ? lastError.error
                      : "");
                return (
                  <tr
                    key={`${id}_${idx}`}
                    className={clickable ? "cursor-pointer hover:bg-white/5" : "hover:bg-white/5"}
                    onClick={() => {
                      if (clickable) onSelectRunId(id);
                    }}
                    title={r.error ? String(r.error) : ""}
                  >
                    <td className="px-3 py-2 font-mono text-white/80">{id > 0 ? id : "-"}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{fmtTs(r.ts_unix_ms)}</td>
                    <td className="px-3 py-2">{r.tools ?? ""}</td>
                    <td className="px-3 py-2">{r.model ?? ""}</td>
                    <td className="px-3 py-2">
                      {ok === undefined ? (
                        ""
                      ) : ok ? (
                        <span className="text-emerald-200/80">true</span>
                      ) : (
                        <span className="text-amber-200/80">false</span>
                      )}
                    </td>
                    <td className="px-3 py-2 text-white/60">{stopReason}</td>
                    <td className="px-3 py-2 text-white/60">{steps}</td>
                    <td className="px-3 py-2 text-white/60">{toolCalls}</td>
                    <td className="px-3 py-2 text-white/60">{lastReason}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </div>
    </div>
  );
}
