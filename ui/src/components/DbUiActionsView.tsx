import React from "react";
import type { DbUiActionRow } from "../history/historyPanelData";

function fmtTs(ts?: number) {
  if (typeof ts !== "number" || !Number.isFinite(ts) || ts <= 0) return "";
  try {
    return new Date(ts).toISOString();
  } catch {
    return "";
  }
}

function clip(s: string, max = 100) {
  const t = String(s ?? "");
  if (t.length <= max) return t;
  return `${t.slice(0, max - 1)}…`;
}

export default function DbUiActionsView({
  uiActions,
  onSelectRunId,
}: {
  uiActions: DbUiActionRow[];
  onSelectRunId?: (runId: number) => void;
}) {
  return (
    <div className="mt-2 rounded-md border border-white/10 bg-black/30">
      <div className="flex items-center justify-between border-b border-white/10 px-3 py-2">
        <div className="text-xs text-white/70">DB ui_actions</div>
        <div className="text-[11px] text-white/40">{uiActions.length} rows</div>
      </div>
      <div className="max-h-[240px] overflow-auto">
        {uiActions.length === 0 ? (
          <div className="px-3 py-3 text-[11px] text-white/40">(no rows)</div>
        ) : (
          <table className="w-full text-left text-[11px] text-white/70">
            <thead className="sticky top-0 bg-black/40 text-white/50">
              <tr>
                <th className="px-3 py-2">id</th>
                <th className="px-3 py-2">run</th>
                <th className="px-3 py-2">ts</th>
                <th className="px-3 py-2">type</th>
                <th className="px-3 py-2">title</th>
                <th className="px-3 py-2">message</th>
                <th className="px-3 py-2">path</th>
                <th className="px-3 py-2">repeat</th>
                <th className="px-3 py-2">autoplay</th>
              </tr>
            </thead>
            <tbody>
              {uiActions.map((a, idx) => {
                const id = typeof a.id === "number" ? a.id : 0;
                const runId = typeof a.run_id === "number" ? a.run_id : 0;
                const clickable = runId > 0 && typeof onSelectRunId === "function";
                const typ = a.type ? String(a.type) : "";
                const title = a.title ? String(a.title) : "";
                const message = a.message ? String(a.message) : "";
                const path = a.path ? String(a.path) : "";
                const repeat = typeof a.repeat === "number" ? a.repeat : "";
                const autoplay = typeof a.autoplay === "boolean" ? a.autoplay : undefined;
                const tooltipParts: string[] = [];
                if (a.tool_call_id) tooltipParts.push(`tool_call_id=${a.tool_call_id}`);
                if (a.mime) tooltipParts.push(`mime=${a.mime}`);
                if (a.action_json) tooltipParts.push(`action_json=${clip(String(a.action_json), 240)}`);
                return (
                  <tr
                    key={`${id}_${idx}`}
                    className={clickable ? "cursor-pointer hover:bg-white/5" : "hover:bg-white/5"}
                    onClick={() => {
                      if (clickable) onSelectRunId(runId);
                    }}
                    title={tooltipParts.join("\n")}
                  >
                    <td className="px-3 py-2 font-mono text-white/80">{id > 0 ? id : "-"}</td>
                    <td className="px-3 py-2 font-mono text-white/70">{runId > 0 ? runId : "-"}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{fmtTs(a.ts_unix_ms)}</td>
                    <td className="px-3 py-2 text-white/60">{clip(typ, 40)}</td>
                    <td className="px-3 py-2 text-white/60">{clip(title, 80)}</td>
                    <td className="px-3 py-2 text-white/60">{clip(message, 90)}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{clip(path, 60)}</td>
                    <td className="px-3 py-2 text-white/60">{repeat}</td>
                    <td className="px-3 py-2 text-white/60">{autoplay === undefined ? "" : autoplay ? "true" : "false"}</td>
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
