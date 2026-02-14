import React from "react";
import type { ApiAuth } from "../api";
import ConversationView from "./ConversationView";
import Markdown from "./Markdown";
import type { SceneEntity } from "./SceneView";

export type HistoryPanelProps = {
  entries: any[];
  showAllEntries: boolean;
  setShowAllEntries: React.Dispatch<React.SetStateAction<boolean>>;
  showMessages: boolean;
  setShowMessages: React.Dispatch<React.SetStateAction<boolean>>;
  historyExpandedByKey: Record<string, boolean>;
  setHistoryExpandedByKey: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  effectiveBase: string;
  yolo: boolean;
  sessionId: string;
  client: { id: string; kind: string; instance_id: string };
  daemonAuth: ApiAuth;
  showDebugInConversation: boolean;
  allowAutoplay: boolean;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  sceneEntities: SceneEntity[];
  onSceneApply: (ops: any[]) => void;
  onTraceIdClick: (traceId: string) => void;
};

export default function HistoryPanel(props: HistoryPanelProps) {
  const entries = Array.isArray(props.entries) ? props.entries : [];
  const MAX_HISTORY_EXPANDED_KEYS = 200;

  const entryKeyFor = React.useCallback((entry: any) => {
    const isLive = entry?.live === true;
    const jobId = typeof entry?.job_id === "string" ? entry.job_id : "";
    const ts = typeof entry?.ts_unix_ms === "number" ? entry.ts_unix_ms : 0;
    return isLive && jobId ? `job:${jobId}` : `ts:${String(ts || 0)}`;
  }, []);

  const expandedKeyCandidates = React.useMemo(() => {
    const keys: string[] = [];
    for (const e of entries) {
      keys.push(entryKeyFor(e));
      if (keys.length >= MAX_HISTORY_EXPANDED_KEYS) break;
    }
    return keys;
  }, [entries, entryKeyFor]);

  React.useEffect(() => {
    if (expandedKeyCandidates.length === 0) return;
    const keep = new Set(expandedKeyCandidates);
    props.setHistoryExpandedByKey((prev) => {
      const cur = prev || {};
      let changed = false;
      const next: Record<string, boolean> = {};
      for (const key of Object.keys(cur)) {
        if (keep.has(key)) next[key] = cur[key];
        else changed = true;
      }
      return changed ? next : cur;
    });
  }, [expandedKeyCandidates, props.setHistoryExpandedByKey]);

  return (
    <div>
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-sm font-semibold text-white/80">History</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => props.setShowMessages((v) => !v)}
            title={props.showMessages ? "Hide per-run event transcript" : "Show per-run event transcript"}
          >
            {props.showMessages ? "Hide messages" : "Show messages"}
          </button>
          {entries.length > 1 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.setShowAllEntries((v) => !v)}
              title={props.showAllEntries ? "Hide older history entries" : "Show older history entries"}
            >
              {props.showAllEntries ? `Hide history (${entries.length - 1})` : `Show history (${entries.length - 1})`}
            </button>
          ) : null}
        </div>
      </div>

      {entries.length === 0 ? (
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
          No history yet. Run a prompt to populate the timeline.
        </div>
      ) : (
        <div className="grid gap-2">
          {(props.showAllEntries ? entries : entries.slice(0, 1)).map((e: any, idx: number) => {
            const ts = typeof e?.ts_unix_ms === "number" ? e.ts_unix_ms : 0;
            const when = ts ? new Date(ts).toLocaleString() : "";
            const promptText = typeof e?.prompt === "string" ? e.prompt : "";
            const assistantText = typeof e?.assistant_text === "string" ? e.assistant_text : "";
            const evs = Array.isArray(e?.events) ? (e.events as any[]) : [];
            const ok = typeof e?.ok === "boolean" ? e.ok : undefined;
            const isLive = e?.live === true;
            const jobId = typeof e?.job_id === "string" ? e.job_id : "";
            const jobSt = typeof e?.job_status === "string" ? e.job_status : "";
            const traceId = typeof e?.trace_id === "string" ? e.trace_id : "";
            const status = ok === true ? "ok" : ok === false ? "error" : "";
            const summary = promptText.trim().length > 0 ? promptText.trim().slice(0, 200) : "(no prompt)";
            const entryKey = entryKeyFor(e);
            const expanded =
              Object.prototype.hasOwnProperty.call(props.historyExpandedByKey, entryKey)
                ? !!props.historyExpandedByKey[entryKey]
                : idx === 0;

            return (
              <details
                key={entryKey}
                open={expanded}
                onToggle={(ev) => {
                  const open = (ev.currentTarget as HTMLDetailsElement).open;
                  props.setHistoryExpandedByKey((prev) => ({ ...(prev || {}), [entryKey]: open }));
                }}
                className="rounded-lg border border-white/10 bg-white/5 px-3 py-2"
              >
                <summary className="cursor-pointer select-none text-xs text-white/80">
                  <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
                    <span className="shrink-0 text-white/60">{when}</span>
                    {isLive ? (
                      <span className="shrink-0 text-indigo-300">
                        running{jobSt ? ` (${jobSt})` : ""}
                        {jobId ? (
                          <>
                            {" "}
                            <code className="inline-block max-w-[50vw] truncate align-bottom text-indigo-200/80" title={jobId}>
                              {jobId}
                            </code>
                          </>
                        ) : null}
                      </span>
                    ) : null}
                    {status ? (
                      <span className={`${status === "ok" ? "text-emerald-300" : "text-rose-300"}`}>{status}</span>
                    ) : null}
                    {traceId ? (
                      <button
                        className="shrink-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                        type="button"
                        title="Open trace lookup for this run"
                        onClick={(ev) => {
                          ev.preventDefault();
                          ev.stopPropagation();
                          props.onTraceIdClick(traceId);
                        }}
                      >
                        Trace
                      </button>
                    ) : null}
                    <span className="min-w-0 flex-1">{summary}</span>
                    <span className="shrink-0 text-white/40">({evs.length} events)</span>
                  </div>
                </summary>
                <div className="mt-3 grid gap-3">
                  {assistantText ? (
                    <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
                      <div className="flex items-start gap-2">
                        <div className="shrink-0 text-[11px] font-semibold text-white/60">Assistant</div>
                        <div className="min-w-0 flex-1">
                          <Markdown text={assistantText} />
                        </div>
                      </div>
                    </div>
                  ) : null}
                  {evs.length > 0 && (isLive || props.showMessages) ? (
                    <ConversationView
                      baseUrl={props.effectiveBase}
                      yolo={props.yolo}
                      sessionId={props.sessionId}
                      client={props.client}
                      daemonAuth={props.daemonAuth}
                      prompt={promptText}
                      events={evs as any}
                      showDebugEvents={props.showDebugInConversation}
                      allowAutoplay={props.allowAutoplay}
                      allowClientRpcs={props.allowClientRpcs}
                      allowClientEffects={props.allowClientEffects}
                      allowUnsafePageEval={props.allowUnsafePageEval}
                      reverseOrder={true}
                      disableAutoClientRpcs={idx !== 0}
                      sceneEntities={props.sceneEntities}
                      onSceneApply={props.onSceneApply}
                    />
                  ) : null}
                </div>
              </details>
            );
          })}
        </div>
      )}
    </div>
  );
}
