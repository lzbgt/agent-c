import React from "react";
import Markdown from "../Markdown";
import type { ApiAuth } from "../../api";
import { safeObject, type UnknownRecord } from "../../jsonUtils";
import type { useToolResultViewState } from "./useToolResultViewState";
import {
  DiffBlock,
  EntriesView,
  firstNLines,
  looksLikeMarkdown,
  normalizeToolResultEntries,
  normalizeToolResultSearchMatches,
  SearchMatchesView,
} from "./toolResultUtils";

type ToolResultParsedViewProps = {
  content: string;
  daemonAuth?: ApiAuth;
  parsed: UnknownRecord;
  sessionId?: string;
  state: ReturnType<typeof useToolResultViewState>;
  yolo: boolean;
};

export default function ToolResultParsedView(props: ToolResultParsedViewProps) {
  const { parsed, state } = props;
  const data = safeObject(parsed.data);
  const apply = safeObject(data.apply);
  const toolName = typeof data.tool === "string" ? data.tool : "";
  const patch = typeof data.patch === "string" ? data.patch : null;
  const output = typeof data.output === "string" ? data.output : null;
  const hasOutput = output !== null;
  const cmd = typeof data.cmd === "string" ? data.cmd : null;
  const argv = Array.isArray(data.argv)
    ? data.argv.filter((value): value is string => typeof value === "string" && value.length > 0)
    : null;
  const matches = normalizeToolResultSearchMatches(data.matches);
  const entries = normalizeToolResultEntries(data.entries);
  const toolPath = typeof data.path === "string" ? data.path : null;
  const exitCode =
    typeof data.exit_code === "number"
      ? data.exit_code
      : typeof apply.exit_code === "number"
        ? apply.exit_code
        : null;
  const ok = typeof parsed.ok === "boolean" ? parsed.ok : null;
  const error = typeof parsed.error === "string" ? parsed.error : null;
  const protocolViolation = parsed.protocol_violation === true;
  const timedOut = typeof data.timed_out === "boolean" ? data.timed_out : null;
  const waitForType = typeof data.wait_for_type === "string" ? data.wait_for_type : null;
  const waitTimeoutMs = typeof data.timeout_ms === "number" ? data.timeout_ms : null;
  const lastType = typeof data.last_type === "string" ? data.last_type : null;
  const lastTsUnixMs = typeof data.last_ts_unix_ms === "number" ? data.last_ts_unix_ms : null;

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
    state.renderMode === "markdown"
      ? "markdown"
      : state.renderMode === "text"
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
        {protocolViolation ? <span className="rounded-md bg-rose-500/10 px-2 py-0.5 text-rose-200">protocol_violation</span> : null}
        {error ? <span className="rounded-md bg-rose-500/10 px-2 py-0.5 text-rose-200">{error}</span> : null}

        {hasOutput ? (
          <div className="ml-auto flex items-center gap-2">
            {outputIsLong ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                onClick={() => state.setShowFullOutput(!state.showFullOutput)}
                type="button"
              >
                {state.showFullOutput ? "Collapse output" : "Expand output"}
              </button>
            ) : null}
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
              value={state.renderMode}
              onChange={(event) => state.setRenderMode(event.target.value as "auto" | "text" | "markdown")}
            >
              <option value="auto">auto</option>
              <option value="text">text</option>
              <option value="markdown">markdown</option>
            </select>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              onClick={() => state.setShowRaw(!state.showRaw)}
              type="button"
            >
              {state.showRaw ? "Hide raw" : "Show raw"}
            </button>
          </div>
        ) : (
          <button
            className="ml-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            onClick={() => state.setShowRaw(!state.showRaw)}
            type="button"
          >
            {state.showRaw ? "Hide raw" : "Show raw"}
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

      {protocolViolation ? (
        <div className="mb-2 rounded-md border border-rose-400/30 bg-rose-500/10 px-3 py-2 text-[11px] text-rose-100/90">
          <div className="font-semibold">Tool server protocol violation</div>
          <div className="mt-1 text-rose-100/80">
            The tool server returned malformed JSON or exceeded the response size cap. Agentd treats this as a hard
            protocol error and restarts the server before the next call.
          </div>
        </div>
      ) : null}

      {hasOutput ? (
        <div>
          {toolName === "shell_exec" && cmd ? (
            <div className="mb-2 rounded-md border border-white/10 bg-black/20 p-2">
              <div className="mb-1 text-[11px] font-semibold text-white/70">Command</div>
              <pre className="overflow-auto whitespace-pre-wrap break-words font-mono text-[11px] leading-relaxed text-white/90">
                cmd: {cmd}
              </pre>
            </div>
          ) : toolName === "proc_exec" && argv ? (
            <div className="mb-2 rounded-md border border-white/10 bg-black/20 p-2">
              <div className="mb-1 text-[11px] font-semibold text-white/70">Command</div>
              <pre className="overflow-auto whitespace-pre-wrap break-words font-mono text-[11px] leading-relaxed text-white/90">
                argv: {argv.join(" ")}
              </pre>
            </div>
          ) : null}
          {peek && outputIsLong ? (
            <div className="mb-2">
              <div className="mb-1 text-[11px] text-white/60">
                Peek (first 5 lines){peek.totalLines > 5 ? ` of ${peek.totalLines}` : ""}:
              </div>
              <pre
                className="cursor-pointer overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90 hover:bg-black/35"
                title="Click to expand/collapse output"
                onClick={() => state.setShowFullOutput(!state.showFullOutput)}
              >
                {peek.head}
              </pre>
            </div>
          ) : null}

          {!outputIsLong || state.showFullOutput ? (
            effectiveMode === "markdown" ? (
              <div className="rounded-md border border-white/10 bg-black/20 p-2">
                <Markdown text={output ?? ""} />
              </div>
            ) : (
              <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
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

      {toolName === "text_search" && matches.length > 0 ? <SearchMatchesView matches={matches} /> : null}
      {(toolName === "fs_list" || toolName === "fs_find") && entries.length > 0 ? (
        <EntriesView entries={entries} title={toolName === "fs_find" ? "Found" : "Entries"} />
      ) : null}

      {state.showRaw ? (
        <div className="mt-2">
          <div className="mb-1 text-xs font-semibold text-white/70">Raw JSON</div>
          <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
            {JSON.stringify(parsed, null, 2)}
          </pre>
        </div>
      ) : null}
    </div>
  );
}
