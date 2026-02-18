import React from "react";

type DbMessageRow = {
  id?: number;
  idx?: number;
  role?: string;
  content?: string;
  content_truncated?: boolean;
  content_bytes?: number;
  mm_json?: string;
  mm_json_truncated?: boolean;
  mm_bytes?: number;
  mm_truncated?: number;
  created_unix_ms?: number;
};

function fmtTs(ts?: number) {
  if (typeof ts !== "number" || !Number.isFinite(ts) || ts <= 0) return "";
  try {
    return new Date(ts).toISOString();
  } catch {
    return "";
  }
}

function clip(s: string, max = 200) {
  const t = String(s ?? "");
  if (t.length <= max) return t;
  return `${t.slice(0, max - 1)}…`;
}

function mmLabel(m: DbMessageRow) {
  const mmJson = typeof m.mm_json === "string" ? m.mm_json : "";
  const mmBytes = typeof m.mm_bytes === "number" ? m.mm_bytes : mmJson.length;
  const hasMm = mmBytes > 0 || mmJson.length > 0;
  if (!hasMm) return "";
  const truncated = m.mm_json_truncated ? true : false;
  const storedTrunc = m.mm_truncated === 1;
  const parts: string[] = [];
  if (truncated) parts.push("resp_trunc");
  if (storedTrunc) parts.push("stored_trunc");
  parts.push(`${mmBytes}b`);
  return parts.join(" ");
}

export default function DbMessagesView({ messages }: { messages: DbMessageRow[] }) {
  return (
    <div className="mt-2 rounded-md border border-white/10 bg-black/30">
      <div className="flex items-center justify-between border-b border-white/10 px-3 py-2">
        <div className="text-xs text-white/70">DB messages</div>
        <div className="text-[11px] text-white/40">{messages.length} rows</div>
      </div>
      <div className="max-h-[260px] overflow-auto">
        {messages.length === 0 ? (
          <div className="px-3 py-3 text-[11px] text-white/40">(no rows)</div>
        ) : (
          <table className="w-full text-left text-[11px] text-white/70">
            <thead className="sticky top-0 bg-black/40 text-white/50">
              <tr>
                <th className="px-3 py-2">idx</th>
                <th className="px-3 py-2">role</th>
                <th className="px-3 py-2">ts</th>
                <th className="px-3 py-2">mm</th>
                <th className="px-3 py-2">content</th>
              </tr>
            </thead>
            <tbody>
              {messages.map((m, i) => {
                const idx = typeof m.idx === "number" ? m.idx : 0;
                const role = m.role ? String(m.role) : "";
                const created = typeof m.created_unix_ms === "number" ? m.created_unix_ms : undefined;
                const content = m.content ? String(m.content) : "";
                const truncated = m.content_truncated ? true : false;
                const bytes = typeof m.content_bytes === "number" ? m.content_bytes : undefined;
                const titleParts: string[] = [];
                if (truncated) titleParts.push("truncated");
                if (typeof bytes === "number") titleParts.push(`bytes=${bytes}`);
                const mm = mmLabel(m);
                return (
                  <tr key={`${idx}_${i}`} className="hover:bg-white/5" title={titleParts.join(" ")}>
                    <td className="px-3 py-2 font-mono text-white/80">{idx}</td>
                    <td className="px-3 py-2 text-white/60">{role}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{fmtTs(created)}</td>
                    <td className="px-3 py-2 font-mono text-white/60">{mm}</td>
                    <td className="px-3 py-2 text-white/60">{clip(content, 260)}</td>
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
