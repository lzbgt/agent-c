import React from "react";

export type ToolResultUiPrefs = {
  showRaw?: boolean;
  showFullOutput?: boolean;
  renderMode?: "auto" | "text" | "markdown";
};

export function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

export function looksLikeMarkdown(s: string) {
  if (s.includes("```")) return true;
  if (/^#{1,6}\s/m.test(s)) return true;
  if (/^\s*-\s+/m.test(s)) return true;
  if (/\[[^\]]+\]\([^)]+\)/.test(s)) return true;
  return false;
}

export function firstNLines(s: string, n: number): { head: string; totalLines: number } {
  const lines = String(s || "").split("\n");
  const head = lines.slice(0, Math.max(0, n)).join("\n");
  return { head, totalLines: lines.length };
}

export function DiffBlock({ text }: { text: string }) {
  const lines = text.split("\n");
  return (
    <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-indigo-400/20 bg-indigo-500/10 p-3 text-xs leading-relaxed text-indigo-50">
      {lines.map((line, index) => {
        const klass =
          line.startsWith("+") && !line.startsWith("+++")
            ? "text-emerald-200"
            : line.startsWith("-") && !line.startsWith("---")
              ? "text-rose-200"
              : line.startsWith("@@")
                ? "text-amber-200"
                : "";
        return (
          <div key={index} className={klass}>
            {line}
          </div>
        );
      })}
    </pre>
  );
}

export function SearchMatchesView({ matches }: { matches: any[] }) {
  const items = Array.isArray(matches) ? matches : [];
  if (items.length === 0) return null;
  return (
    <div className="mt-3">
      <div className="mb-1 text-xs font-semibold text-white/70">Matches</div>
      <div className="overflow-auto rounded-md border border-white/10 bg-black/20">
        <table className="w-full text-left text-xs text-white/85">
          <thead className="sticky top-0 bg-black/40 text-white/70">
            <tr>
              <th className="px-2 py-2">File</th>
              <th className="px-2 py-2">Line</th>
              <th className="px-2 py-2">Col</th>
              <th className="px-2 py-2">Snippet</th>
            </tr>
          </thead>
          <tbody>
            {items.slice(0, 200).map((match, index) => (
              <tr key={index} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof match?.path === "string" ? match.path : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof match?.line === "number" ? match.line : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof match?.column === "number" ? match.column : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] whitespace-pre-wrap">
                  {typeof match?.snippet === "string" ? match.snippet : ""}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export function EntriesView({ entries, title }: { entries: any[]; title?: string }) {
  const items = Array.isArray(entries) ? entries : [];
  if (items.length === 0) return null;
  return (
    <div className="mt-3">
      <div className="mb-1 text-xs font-semibold text-white/70">{title ?? "Entries"}</div>
      <div className="overflow-auto rounded-md border border-white/10 bg-black/20">
        <table className="w-full text-left text-xs text-white/85">
          <thead className="sticky top-0 bg-black/40 text-white/70">
            <tr>
              <th className="px-2 py-2">Path</th>
              <th className="px-2 py-2">Type</th>
              <th className="px-2 py-2">Size</th>
            </tr>
          </thead>
          <tbody>
            {items.slice(0, 200).map((entry, index) => (
              <tr key={index} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof entry?.path === "string" ? entry.path : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof entry?.type === "string" ? entry.type : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof entry?.size_bytes === "number" ? entry.size_bytes : ""}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
