import React from "react";
import { safeObject, safeJsonParse } from "../../jsonUtils";

export { safeJsonParse };

export type ToolResultUiPrefs = {
  showRaw?: boolean;
  showFullOutput?: boolean;
  renderMode?: "auto" | "text" | "markdown";
};

export type ToolResultSearchMatch = {
  path: string;
  line?: number;
  column?: number;
  snippet: string;
};

export type ToolResultEntry = {
  path: string;
  type?: string;
  size_bytes?: number;
};

export function normalizeToolResultSearchMatches(matches: unknown): ToolResultSearchMatch[] {
  if (!Array.isArray(matches)) return [];
  return matches
    .map((matchValue) => {
      const match = safeObject(matchValue);
      const path = typeof match.path === "string" ? match.path : "";
      const line = typeof match.line === "number" ? match.line : undefined;
      const column = typeof match.column === "number" ? match.column : undefined;
      const snippet = typeof match.snippet === "string" ? match.snippet : "";
      return { path, line, column, snippet };
    })
    .filter((match) => match.path.length > 0 || match.snippet.length > 0);
}

export function normalizeToolResultEntries(entries: unknown): ToolResultEntry[] {
  if (!Array.isArray(entries)) return [];
  return entries
    .map((entryValue) => {
      const entry = safeObject(entryValue);
      return {
        path: typeof entry.path === "string" ? entry.path : "",
        type: typeof entry.type === "string" ? entry.type : undefined,
        size_bytes: typeof entry.size_bytes === "number" ? entry.size_bytes : undefined,
      };
    })
    .filter((entry) => entry.path.length > 0);
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

export function SearchMatchesView({ matches }: { matches: ToolResultSearchMatch[] }) {
  if (matches.length === 0) return null;
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
            {matches.slice(0, 200).map((match, index) => (
              <tr key={index} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{match.path}</td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{match.line ?? ""}</td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{match.column ?? ""}</td>
                <td className="px-2 py-2 font-mono text-[11px] whitespace-pre-wrap">{match.snippet}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export function EntriesView({ entries, title }: { entries: ToolResultEntry[]; title?: string }) {
  if (entries.length === 0) return null;
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
            {entries.slice(0, 200).map((entry, index) => (
              <tr key={index} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{entry.path}</td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{entry.type ?? ""}</td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">{entry.size_bytes ?? ""}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
