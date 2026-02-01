import React from "react";
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

function firstNLines(s: string, n: number): { head: string; totalLines: number } {
  const lines = String(s || "").split("\n");
  const head = lines.slice(0, Math.max(0, n)).join("\n");
  return { head, totalLines: lines.length };
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
  daemonAuthToken,
  content,
}: {
  baseUrl: string;
  yolo: boolean;
  daemonAuthToken?: string;
  content: string;
}) {
  const [showRaw, setShowRaw] = React.useState(false);
  const [renderMode, setRenderMode] = React.useState<"auto" | "text" | "markdown">("auto");
  const [showFullOutput, setShowFullOutput] = React.useState<boolean>(false);

  const parsed = safeJsonParse(content);
  if (parsed && typeof parsed === "object") {
    const toolName = typeof parsed?.data?.tool === "string" ? parsed.data.tool : "";
    const patch = typeof parsed?.data?.patch === "string" ? parsed.data.patch : null;
    const hasOutput = typeof parsed?.data?.output === "string";
    const output = hasOutput ? (parsed.data.output as string) : null;
    const cmd = typeof parsed?.data?.cmd === "string" ? parsed.data.cmd : null;
    const argv = Array.isArray(parsed?.data?.argv) ? parsed.data.argv : null;
    const matches = Array.isArray(parsed?.data?.matches) ? parsed.data.matches : null;
    const entries = Array.isArray(parsed?.data?.entries) ? parsed.data.entries : null;
    const toolPath = typeof parsed?.data?.path === "string" ? parsed.data.path : null;
    const exitCode =
      typeof parsed?.data?.exit_code === "number"
        ? parsed.data.exit_code
        : typeof parsed?.data?.apply?.exit_code === "number"
          ? parsed.data.apply.exit_code
          : null;
    const ok = typeof parsed?.ok === "boolean" ? parsed.ok : null;
    const error = typeof parsed?.error === "string" ? parsed.error : null;
    const timedOut = typeof parsed?.data?.timed_out === "boolean" ? parsed.data.timed_out : null;
    const waitForType = typeof parsed?.data?.wait_for_type === "string" ? String(parsed.data.wait_for_type) : null;
    const waitTimeoutMs = typeof parsed?.data?.timeout_ms === "number" ? parsed.data.timeout_ms : null;
    const lastType = typeof parsed?.data?.last_type === "string" ? String(parsed.data.last_type) : null;
    const lastTsUnixMs = typeof parsed?.data?.last_ts_unix_ms === "number" ? parsed.data.last_ts_unix_ms : null;

    const isWaitTool = new Set([
      "client_wait_event",
      "client_wait_any",
      "client_wait_all",
      "ui_wait_event",
      "ui_wait_any",
      "ui_wait_all",
    ]).has(toolName);
    const isTimeout = !!isWaitTool && ok === false && (error === "timeout" || timedOut === true);

    const peek = output !== null ? firstNLines(output, 5) : null;
    const outputIsLong = output !== null && (((peek?.totalLines ?? 0) > 5) || output.length > 2000);

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
        <div className="mb-1 flex flex-wrap items-center gap-2 text-[11px] text-white/70">
          {toolName ? <span className="rounded-md bg-white/10 px-2 py-0.5">{toolName}</span> : null}
          {ok !== null ? (
            <span
              className={`rounded-md px-2 py-0.5 ${
                ok ? "bg-emerald-500/15 text-emerald-200" : "bg-rose-500/15 text-rose-200"
              }`}
            >
              {ok ? "ok" : "error"}
            </span>
          ) : null}
          {exitCode !== null ? <span className="rounded-md bg-white/10 px-2 py-0.5">exit_code={exitCode}</span> : null}
          {toolPath ? <span className="rounded-md bg-white/10 px-2 py-0.5">path={toolPath}</span> : null}
          {timedOut === true ? <span className="rounded-md bg-amber-500/10 px-2 py-0.5 text-amber-200">timed_out</span> : null}
          {error ? <span className="rounded-md bg-rose-500/10 px-2 py-0.5 text-rose-200">{error}</span> : null}

          {hasOutput ? (
            <div className="ml-auto flex items-center gap-2">
              {outputIsLong ? (
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  onClick={() => setShowFullOutput((v) => !v)}
                  type="button"
                >
                  {showFullOutput ? "Collapse output" : "Expand output"}
                </button>
              ) : null}
              <select
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                value={renderMode}
                onChange={(e) => setRenderMode(e.target.value as any)}
              >
                <option value="auto">auto</option>
                <option value="text">text</option>
                <option value="markdown">markdown</option>
              </select>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                onClick={() => setShowRaw((v) => !v)}
                type="button"
              >
                {showRaw ? "Hide raw" : "Show raw"}
              </button>
            </div>
          ) : (
            <button
              className="ml-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              onClick={() => setShowRaw((v) => !v)}
              type="button"
            >
              {showRaw ? "Hide raw" : "Show raw"}
            </button>
          )}
        </div>

        {isTimeout ? (
          <div className="mb-2 rounded-md border border-amber-400/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100/90">
            <div className="font-semibold">Timed out waiting for an event</div>
            {waitForType || waitTimeoutMs !== null ? (
              <div className="mt-1 font-mono text-[11px] text-amber-100/80">
                wait_for_type={waitForType || "(unknown)"} timeout_ms={waitTimeoutMs ?? "(unknown)"}
                {lastType ? ` last_type=${lastType}` : ""}
                {lastTsUnixMs ? ` last_ts_unix_ms=${lastTsUnixMs}` : ""}
              </div>
            ) : null}
            <div className="mt-1 text-amber-100/80">
              The client may not have been connected (or may have missed the request). If this was a client RPC flow, prefer
              requesting a deterministic follow-up RPC (dom_query/entity_query/state_snapshot) instead of looping on retries.
            </div>
          </div>
        ) : null}

        {hasOutput ? (
          <div>
            {toolName === "shell_exec" && cmd ? (
              <div className="mb-2 rounded-md border border-white/10 bg-black/20 p-2">
                <div className="mb-1 text-[11px] font-semibold text-white/70">Command</div>
                <pre className="overflow-auto whitespace-pre-wrap font-mono text-[11px] leading-relaxed text-white/90">
                  cmd: {cmd}
                </pre>
              </div>
            ) : toolName === "proc_exec" && argv ? (
              <div className="mb-2 rounded-md border border-white/10 bg-black/20 p-2">
                <div className="mb-1 text-[11px] font-semibold text-white/70">Command</div>
                <pre className="overflow-auto whitespace-pre-wrap font-mono text-[11px] leading-relaxed text-white/90">
                  argv: {(argv as any[]).map((x) => (typeof x === "string" ? x : "")).filter((x) => x.length > 0).join(" ")}
                </pre>
              </div>
            ) : null}
            {peek && outputIsLong ? (
              <div className="mb-2">
                <div className="mb-1 text-[11px] text-white/60">
                  Peek (first 5 lines){peek.totalLines > 5 ? ` of ${peek.totalLines}` : ""}:
                </div>
                <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                  {peek.head}
                </pre>
              </div>
            ) : null}

            {!outputIsLong || showFullOutput ? (
              effectiveMode === "markdown" ? (
                <div className="rounded-md border border-white/10 bg-black/20 p-2">
                  <Markdown text={output ?? ""} />
                </div>
              ) : (
                <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                  {output ?? ""}
                </pre>
              )
            ) : (
              <div className="text-[11px] text-white/50">Output collapsed. Expand to view full content.</div>
            )}
          </div>
        ) : null}

        {typeof patch === "string" ? (
          <details className="mt-2 rounded-md border border-white/10 bg-black/20 px-3 py-2">
            <summary className="cursor-pointer select-none text-xs font-semibold text-white/70">Diff</summary>
            <div className="mt-2">
              <DiffBlock text={patch} />
            </div>
          </details>
        ) : null}

        {toolName === "text_search" && matches ? <SearchMatchesView matches={matches} /> : null}
        {(toolName === "fs_list" || toolName === "fs_find") && entries ? (
          <EntriesView entries={entries} title={toolName === "fs_find" ? "Found" : "Entries"} />
        ) : null}

        {showRaw ? (
          <div className="mt-2">
            <div className="mb-1 text-xs font-semibold text-white/70">Raw JSON</div>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
              {JSON.stringify(parsed, null, 2)}
            </pre>
          </div>
        ) : null}
      </div>
    );
  }

  return (
    <div>
      {(() => {
        const peek = firstNLines(content, 5);
        const isLong = peek.totalLines > 5 || content.length > 2000;
        return (
          <div>
            {isLong ? (
              <div className="mb-2">
                <div className="mb-1 text-[11px] text-white/60">
                  Peek (first 5 lines){peek.totalLines > 5 ? ` of ${peek.totalLines}` : ""}:
                </div>
                <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                  {peek.head}
                </pre>
              </div>
            ) : null}

            {!isLong || showFullOutput ? (
              <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                {content}
              </pre>
            ) : (
              <div className="text-[11px] text-white/50">Output collapsed. Expand to view full content.</div>
            )}

            {isLong ? (
              <button
                className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
                onClick={() => setShowFullOutput((v) => !v)}
                type="button"
              >
                {showFullOutput ? "Collapse output" : "Expand output"}
              </button>
            ) : null}
          </div>
        );
      })()}
    </div>
  );
}
