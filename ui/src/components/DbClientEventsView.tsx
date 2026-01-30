import React from "react";

type DbClientEventRow = {
  id?: number;
  ts_unix_ms?: number;
  type?: string | null;
  data?: any;
  data_json?: string | null;
};

function fmtTs(ts?: number) {
  if (typeof ts !== "number" || !Number.isFinite(ts) || ts <= 0) return "";
  try {
    return new Date(ts).toISOString();
  } catch {
    return "";
  }
}

function clip(s: string, max = 120) {
  const t = String(s ?? "");
  if (t.length <= max) return t;
  return `${t.slice(0, max - 1)}…`;
}

export default function DbClientEventsView({ events }: { events: DbClientEventRow[] }) {
  return (
    <div className="mt-2 rounded-md border border-white/10 bg-black/30">
      <div className="flex items-center justify-between border-b border-white/10 px-3 py-2">
        <div className="text-xs text-white/70">DB client_events</div>
        <div className="text-[11px] text-white/40">{events.length} rows</div>
      </div>
      <div className="max-h-[220px] overflow-auto">
        {events.length === 0 ? (
          <div className="px-3 py-3 text-[11px] text-white/40">(no rows)</div>
        ) : (
          <table className="w-full text-left text-[11px] text-white/70">
            <thead className="sticky top-0 bg-black/40 text-white/50">
              <tr>
                <th className="px-3 py-2">id</th>
                <th className="px-3 py-2">ts</th>
                <th className="px-3 py-2">type</th>
                <th className="px-3 py-2">data</th>
              </tr>
            </thead>
            <tbody>
              {events.map((e, idx) => {
                const id = typeof e.id === "number" ? e.id : 0;
                const typ = e.type ? String(e.type) : "";
                const data = e.data && typeof e.data === "object" ? JSON.stringify(e.data) : e.data_json ? String(e.data_json) : "";
                return (
                  <tr key={`${id}_${idx}`} className="hover:bg-white/5" title={data}>
                    <td className="px-3 py-2 font-mono text-white/80">{id > 0 ? id : "-"}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{fmtTs(e.ts_unix_ms)}</td>
                    <td className="px-3 py-2 text-white/60">{clip(typ, 60)}</td>
                    <td className="px-3 py-2 text-white/60">{clip(data, 160)}</td>
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

