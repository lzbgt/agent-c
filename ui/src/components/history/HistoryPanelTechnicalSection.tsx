import React from "react";
import type { ApiAuth } from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import type { SceneEntity } from "../SceneView";
import ConversationView from "../ConversationView";
import Markdown from "../Markdown";

type HistoryPanelTechnicalSectionProps = {
  entries: any[];
  showAllEntries: boolean;
  setShowAllEntries: React.Dispatch<React.SetStateAction<boolean>>;
  showMessages: boolean;
  setShowMessages: React.Dispatch<React.SetStateAction<boolean>>;
  historyExpandedByKey: Record<string, boolean>;
  setHistoryExpandedByKey: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  effectiveBase: string;
  sessionId: string;
  yolo: boolean;
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
  showSystemMessages: boolean;
  setShowSystemMessages: React.Dispatch<React.SetStateAction<boolean>>;
};

const MAX_HISTORY_EXPANDED_KEYS = 200;

export default function HistoryPanelTechnicalSection(props: HistoryPanelTechnicalSectionProps) {
  const technicalHistoryKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const sid = String(props.sessionId || "").trim() || "default";
    return `agentui.technicalHistory:${base}::${sid}`;
  }, [props.effectiveBase, props.sessionId]);
  const [showTechnicalHistory, setShowTechnicalHistory] = useLocalStorageState<boolean>(technicalHistoryKey, false);

  const entryKeyFor = React.useCallback((entry: any) => {
    const isLive = entry?.live === true;
    const jobId = typeof entry?.job_id === "string" ? entry.job_id : "";
    const ts = typeof entry?.ts_unix_ms === "number" ? entry.ts_unix_ms : 0;
    return isLive && jobId ? `job:${jobId}` : `ts:${String(ts || 0)}`;
  }, []);

  const expandedKeyCandidates = React.useMemo(() => {
    const keys: string[] = [];
    for (const entry of props.entries) {
      keys.push(entryKeyFor(entry));
      if (keys.length >= MAX_HISTORY_EXPANDED_KEYS) break;
    }
    return keys;
  }, [entryKeyFor, props.entries]);

  React.useEffect(() => {
    if (expandedKeyCandidates.length === 0) return;
    const keep = new Set(expandedKeyCandidates);
    props.setHistoryExpandedByKey((prev) => {
      const current = prev || {};
      let changed = false;
      const next: Record<string, boolean> = {};
      for (const key of Object.keys(current)) {
        if (keep.has(key)) next[key] = current[key];
        else changed = true;
      }
      return changed ? next : current;
    });
  }, [expandedKeyCandidates, props.setHistoryExpandedByKey]);

  return (
    <>
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-sm font-semibold text-white/80">Technical history</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => props.setShowSystemMessages((value) => !value)}
            title={props.showSystemMessages ? "Hide system messages everywhere" : "Show system messages everywhere"}
          >
            {props.showSystemMessages ? "Hide system" : "Show system"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => setShowTechnicalHistory((value) => !value)}
            title={showTechnicalHistory ? "Hide technical history" : "Show technical history"}
          >
            {showTechnicalHistory ? "Hide technical" : "Show technical"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => props.setShowMessages((value) => !value)}
            title={props.showMessages ? "Hide per-run event transcript" : "Show per-run event transcript"}
          >
            {props.showMessages ? "Hide messages" : "Show messages"}
          </button>
          {props.entries.length > 1 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.setShowAllEntries((value) => !value)}
              title={props.showAllEntries ? "Hide older history entries" : "Show older history entries"}
            >
              {props.showAllEntries ? `Hide history (${props.entries.length - 1})` : `Show history (${props.entries.length - 1})`}
            </button>
          ) : null}
        </div>
      </div>

      {showTechnicalHistory ? (
        props.entries.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No history yet. Run a prompt to populate the timeline.
          </div>
        ) : (
          <div className="grid gap-2">
            {(props.showAllEntries ? props.entries : props.entries.slice(0, 1)).map((entry: any, idx: number) => {
              const ts = typeof entry?.ts_unix_ms === "number" ? entry.ts_unix_ms : 0;
              const when = ts ? new Date(ts).toLocaleString() : "";
              const promptText = typeof entry?.prompt === "string" ? entry.prompt : "";
              const assistantText = typeof entry?.assistant_text === "string" ? entry.assistant_text : "";
              const events = Array.isArray(entry?.events) ? (entry.events as any[]) : [];
              const ok = typeof entry?.ok === "boolean" ? entry.ok : undefined;
              const isLive = entry?.live === true;
              const jobId = typeof entry?.job_id === "string" ? entry.job_id : "";
              const jobStatus = typeof entry?.job_status === "string" ? entry.job_status : "";
              const traceId = typeof entry?.trace_id === "string" ? entry.trace_id : "";
              const status = ok === true ? "ok" : ok === false ? "error" : "";
              const summary = promptText.trim().length > 0 ? promptText.trim().slice(0, 200) : "(no prompt)";
              const entryKey = entryKeyFor(entry);
              const expanded = Object.prototype.hasOwnProperty.call(props.historyExpandedByKey, entryKey)
                ? !!props.historyExpandedByKey[entryKey]
                : idx === 0;
              const tools = typeof entry?.tools === "string" ? entry.tools : "";
              const yolo = typeof entry?.yolo === "boolean" ? entry.yolo : undefined;
              const hostPolicy = typeof entry?.host_policy === "string" ? entry.host_policy : "";
              const automationProfile =
                typeof entry?.effective_automation_profile === "string"
                  ? entry.effective_automation_profile
                  : typeof entry?.automation_profile === "string"
                    ? entry.automation_profile
                    : "";
              const model = typeof entry?.model === "string" ? entry.model : "";
              const baseUrl = typeof entry?.base_url === "string" ? entry.base_url : "";
              const meta = [
                tools ? { label: "tools", value: tools } : null,
                typeof yolo === "boolean" ? { label: "yolo", value: yolo ? "true" : "false" } : null,
                hostPolicy ? { label: "host_policy", value: hostPolicy } : null,
                automationProfile ? { label: "automation_profile", value: automationProfile } : null,
                model ? { label: "model", value: model } : null,
                baseUrl ? { label: "base_url", value: baseUrl } : null,
              ].filter(Boolean) as { label: string; value: string }[];

              return (
                <details
                  key={entryKey}
                  open={expanded}
                  onToggle={(event) => {
                    const open = (event.currentTarget as HTMLDetailsElement).open;
                    props.setHistoryExpandedByKey((prev) => ({ ...(prev || {}), [entryKey]: open }));
                  }}
                  className="rounded-lg border border-white/10 bg-white/5 px-3 py-2"
                >
                  <summary className="cursor-pointer select-none text-xs text-white/80">
                    <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
                      <span className="shrink-0 text-white/60">{when}</span>
                      {isLive ? (
                        <span className="shrink-0 text-indigo-300">
                          {jobId ? "running" : "live session"}
                          {jobStatus ? ` (${jobStatus})` : ""}
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
                      {status ? <span className={status === "ok" ? "text-emerald-300" : "text-rose-300"}>{status}</span> : null}
                      {traceId ? (
                        <button
                          className="shrink-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                          type="button"
                          title="Open trace lookup for this run"
                          onClick={(event) => {
                            event.preventDefault();
                            event.stopPropagation();
                            props.onTraceIdClick(traceId);
                          }}
                        >
                          Trace
                        </button>
                      ) : null}
                      <span className="min-w-0 flex-1">{summary}</span>
                      <span className="shrink-0 text-white/40">({events.length} events)</span>
                    </div>
                  </summary>
                  <div className="mt-3 grid gap-3">
                    {meta.length > 0 ? (
                      <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
                        <div className="mb-1 text-[11px] font-semibold text-white/60">Run settings</div>
                        <div className="flex flex-wrap gap-2">
                          {meta.map((item) => (
                            <div
                              key={`${item.label}:${item.value}`}
                              className="flex items-center gap-1 rounded-md border border-white/10 bg-black/30 px-2 py-1"
                            >
                              <span className="text-white/50">{item.label}</span>
                              <span className="font-mono text-white/80">{item.value}</span>
                            </div>
                          ))}
                        </div>
                      </div>
                    ) : null}
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
                    {events.length > 0 && (isLive || props.showMessages) ? (
                      <ConversationView
                        baseUrl={props.effectiveBase}
                        yolo={props.yolo}
                        sessionId={props.sessionId}
                        client={props.client}
                        daemonAuth={props.daemonAuth}
                        prompt={promptText}
                        events={events as any}
                        showDebugEvents={props.showDebugInConversation}
                        allowAutoplay={props.allowAutoplay}
                        allowClientRpcs={props.allowClientRpcs}
                        allowClientEffects={props.allowClientEffects}
                        allowUnsafePageEval={props.allowUnsafePageEval}
                        reverseOrder={false}
                        disableAutoClientRpcs={idx !== 0 || !jobId}
                        sceneEntities={props.sceneEntities}
                        onSceneApply={props.onSceneApply}
                      />
                    ) : null}
                  </div>
                </details>
              );
            })}
          </div>
        )
      ) : (
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/50">
          Technical history is hidden by default.
        </div>
      )}
    </>
  );
}
