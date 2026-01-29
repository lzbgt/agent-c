import React from "react";
import MediaPreviews from "./MediaPreviews";
import Markdown from "./Markdown";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function DiffBlock({ text }: { text: string }) {
  const lines = text.split("\n");
  return (
    <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-indigo-400/20 bg-indigo-500/10 p-3 text-xs leading-relaxed text-indigo-50">
      {lines.map((l, i) => {
        const klass =
          l.startsWith("+") && !l.startsWith("+++")
            ? "text-emerald-200"
            : l.startsWith("-") && !l.startsWith("---")
              ? "text-rose-200"
              : l.startsWith("@@")
                ? "text-amber-200"
                : "";
        return (
          <div key={i} className={klass}>
            {l}
          </div>
        );
      })}
    </pre>
  );
}

function looksLikeMarkdown(s: string) {
  if (s.includes("```")) return true;
  if (/^#{1,6}\s/m.test(s)) return true;
  if (/^\s*-\s+/m.test(s)) return true;
  if (/\[[^\]]+\]\([^)]+\)/.test(s)) return true;
  return false;
}

function SearchMatchesView({ matches }: { matches: any[] }) {
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
            {items.slice(0, 200).map((m, idx) => (
              <tr key={idx} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof m?.path === "string" ? m.path : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof m?.line === "number" ? m.line : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof m?.column === "number" ? m.column : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] whitespace-pre-wrap">
                  {typeof m?.snippet === "string" ? m.snippet : ""}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

function EntriesView({ entries, title }: { entries: any[]; title?: string }) {
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
            {items.slice(0, 200).map((e, idx) => (
              <tr key={idx} className="border-t border-white/5">
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof e?.path === "string" ? e.path : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof e?.type === "string" ? e.type : ""}
                </td>
                <td className="px-2 py-2 font-mono text-[11px] text-white/80">
                  {typeof e?.size_bytes === "number" ? e.size_bytes : ""}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export default function ToolResultView({
  baseUrl,
  yolo,
  content,
}: {
  baseUrl: string;
  yolo: boolean;
  content: string;
}) {
  const [showRaw, setShowRaw] = React.useState(false);
  const [renderMode, setRenderMode] = React.useState<"auto" | "text" | "markdown">("auto");

  const parsed = safeJsonParse(content);
  if (parsed && typeof parsed === "object") {
    const toolName = typeof parsed?.data?.tool === "string" ? parsed.data.tool : "";
    const patch = typeof parsed?.data?.patch === "string" ? parsed.data.patch : null;
    const output = typeof parsed?.data?.output === "string" ? parsed.data.output : null;
    const matches = Array.isArray(parsed?.data?.matches) ? parsed.data.matches : null;
    const entries = Array.isArray(parsed?.data?.entries) ? parsed.data.entries : null;
    const hasMore = typeof parsed?.data?.has_more === "boolean" ? parsed.data.has_more : null;
    const nextStartLine =
      typeof parsed?.data?.next_start_line === "number" ? parsed.data.next_start_line : null;
    const truncated = typeof parsed?.data?.truncated === "boolean" ? parsed.data.truncated : null;
    const truncatedReason =
      typeof parsed?.data?.truncated_reason === "string" ? parsed.data.truncated_reason : null;
    const toolPath = typeof parsed?.data?.path === "string" ? parsed.data.path : null;
    const exitCode =
      typeof parsed?.data?.exit_code === "number"
        ? parsed.data.exit_code
        : typeof parsed?.data?.apply?.exit_code === "number"
          ? parsed.data.apply.exit_code
          : null;
    const ok = typeof parsed?.ok === "boolean" ? parsed.ok : null;
    const error = typeof parsed?.error === "string" ? parsed.error : null;

    const effectiveMode: "text" | "markdown" =
      renderMode === "markdown"
        ? "markdown"
        : renderMode === "text"
          ? "text"
          : output && looksLikeMarkdown(output)
            ? "markdown"
            : "text";

    return (
      <div>
        <div className="mb-2 flex flex-wrap items-center gap-2 text-xs text-white/70">
          {ok !== null ? (
            <span
              className={`rounded-md px-2 py-1 ${
                ok ? "bg-emerald-500/15 text-emerald-200" : "bg-rose-500/15 text-rose-200"
              }`}
            >
              ok={String(ok)}
            </span>
          ) : null}
          {exitCode !== null ? <span className="rounded-md bg-white/10 px-2 py-1">exit_code={exitCode}</span> : null}
          {toolName ? <span className="rounded-md bg-white/10 px-2 py-1">{toolName}</span> : null}
          {toolPath ? <span className="rounded-md bg-white/10 px-2 py-1">path={toolPath}</span> : null}
          {hasMore !== null ? (
            <span className="rounded-md bg-white/10 px-2 py-1">has_more={String(hasMore)}</span>
          ) : null}
          {nextStartLine !== null ? (
            <span className="rounded-md bg-white/10 px-2 py-1">next_start_line={nextStartLine}</span>
          ) : null}
          {truncated !== null ? (
            <span className="rounded-md bg-white/10 px-2 py-1">truncated={String(truncated)}</span>
          ) : null}
          {truncatedReason ? (
            <span className="rounded-md bg-white/10 px-2 py-1">reason={truncatedReason}</span>
          ) : null}
          {error ? <span className="rounded-md bg-rose-500/10 px-2 py-1 text-rose-200">{error}</span> : null}

          {output ? (
            <div className="ml-auto flex items-center gap-2">
              <select
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/80"
                value={renderMode}
                onChange={(e) => setRenderMode(e.target.value as any)}
              >
                <option value="auto">auto</option>
                <option value="text">text</option>
                <option value="markdown">markdown</option>
              </select>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                onClick={() => setShowRaw((v) => !v)}
                type="button"
              >
                {showRaw ? "Hide raw" : "Show raw"}
              </button>
            </div>
          ) : (
            <button
              className="ml-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              onClick={() => setShowRaw((v) => !v)}
              type="button"
            >
              {showRaw ? "Hide raw" : "Show raw"}
            </button>
          )}
        </div>

        {output ? (
          <div>
            <div className="mb-1 text-xs font-semibold text-white/70">Output</div>
            {effectiveMode === "markdown" ? (
              <div className="rounded-md border border-white/10 bg-black/20 p-3">
                <Markdown text={output} />
              </div>
            ) : (
              <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
                {output}
              </pre>
            )}
            <MediaPreviews baseUrl={baseUrl} yolo={yolo} text={output} />
          </div>
        ) : null}

        {typeof patch === "string" ? (
          <div className="mt-3">
            <div className="mb-1 text-xs font-semibold text-white/70">Diff</div>
            <DiffBlock text={patch} />
          </div>
        ) : null}

        {toolName === "text_search" && matches ? <SearchMatchesView matches={matches} /> : null}
        {(toolName === "fs_list" || toolName === "fs_find") && entries ? (
          <EntriesView entries={entries} title={toolName === "fs_find" ? "Found" : "Entries"} />
        ) : null}

        {showRaw ? (
          <div className="mt-3">
            <div className="mb-1 text-xs font-semibold text-white/70">Raw JSON</div>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(parsed, null, 2)}
            </pre>
          </div>
        ) : null}
      </div>
    );
  }

  return (
    <div>
      <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
        {content}
      </pre>
      <MediaPreviews baseUrl={baseUrl} yolo={yolo} text={content} />
    </div>
  );
}
