import React from "react";
import type { ApiAuth } from "../api";
import useLocalStorageState from "../hooks/useLocalStorageState";
import type { SceneEntity } from "./SceneView";
import ArtifactView from "./ArtifactView";
import Markdown from "./Markdown";
import HistoryPanelTeamSection from "./history/HistoryPanelTeamSection";
import HistoryPanelTechnicalSection from "./history/HistoryPanelTechnicalSection";
import useHistoryPanelTeamState from "./history/useHistoryPanelTeamState";

export type HistoryPanelProps = {
  entries: any[];
  showAllEntries: boolean;
  setShowAllEntries: React.Dispatch<React.SetStateAction<boolean>>;
  showMessages: boolean;
  setShowMessages: React.Dispatch<React.SetStateAction<boolean>>;
  historyExpandedByKey: Record<string, boolean>;
  setHistoryExpandedByKey: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  dbMessages?: any[];
  dbRuns?: any[];
  dbRunDetailsById?: Record<number, any>;
  sessionArtifacts?: any[];
  artifactCatalogMode?: "direct" | "broker_reference" | "unsupported";
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
  teamConversationItems?: any[];
  teamId?: string;
  teamRunId?: string;
  teamRunCreatedMs?: number;
  teamRunStatus?: string;
  teamConversationWarnings?: string[];
};

function formatJson(raw: string): string {
  const text = String(raw ?? "");
  if (!text) return "";
  try {
    const parsed = JSON.parse(text);
    return JSON.stringify(parsed, null, 2);
  } catch {
    return text;
  }
}

export default function HistoryPanel(props: HistoryPanelProps) {
  const entries = Array.isArray(props.entries) ? props.entries : [];
  const dbMessages = Array.isArray(props.dbMessages) ? props.dbMessages : [];
  const dbRuns = Array.isArray(props.dbRuns) ? props.dbRuns : [];
  const dbRunDetailsById =
    props.dbRunDetailsById && typeof props.dbRunDetailsById === "object" ? props.dbRunDetailsById : {};
  const sessionArtifacts = Array.isArray(props.sessionArtifacts) ? props.sessionArtifacts : [];
  const artifactCatalogMode = props.artifactCatalogMode || "direct";
  const teamConversationItems = Array.isArray(props.teamConversationItems) ? props.teamConversationItems : [];
  const teamId = String(props.teamId || "").trim();
  const teamRunId = String(props.teamRunId || "").trim();
  const teamRunCreatedMs = typeof props.teamRunCreatedMs === "number" ? props.teamRunCreatedMs : 0;
  const teamRunStatus = String(props.teamRunStatus || "").trim();
  const teamConversationWarnings = Array.isArray(props.teamConversationWarnings) ? props.teamConversationWarnings : [];

  const systemMessagesKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const sid = String(props.sessionId || "").trim() || "default";
    return `agentui.systemMessages:${base}::${sid}`;
  }, [props.effectiveBase, props.sessionId]);
  const [showSystemMessages, setShowSystemMessages] = useLocalStorageState<boolean>(systemMessagesKey, false);

  const teamState = useHistoryPanelTeamState({
    effectiveBase: props.effectiveBase,
    sessionId: props.sessionId,
    teamId,
    teamConversationItems,
  });

  const conversationItems = React.useMemo(() => {
    const items: {
      kind: "message" | "tool_record";
      ts: number;
      message?: any;
      run?: any;
      runId?: number;
      details?: any;
      toolRecord?: any;
    }[] = [];
    for (const message of dbMessages) {
      const ts = typeof message?.created_unix_ms === "number" ? message.created_unix_ms : 0;
      if (!ts) continue;
      items.push({ kind: "message", ts, message });
    }
    for (const run of dbRuns) {
      const ts = typeof run?.ts_unix_ms === "number" ? run.ts_unix_ms : 0;
      if (!ts) continue;
      const runId = typeof run?.run_id === "number" ? run.run_id : Number(run?.run_id ?? run?.id ?? NaN);
      const details = Number.isFinite(runId) ? dbRunDetailsById[runId] : undefined;
      const safeRunId = Number.isFinite(runId) ? runId : undefined;
      const toolRecords = details && Array.isArray(details?.tool_records) ? (details.tool_records as any[]) : [];
      toolRecords.forEach((toolRecord, idx) => {
        const toolTs =
          typeof toolRecord?.ts_unix_ms === "number"
            ? toolRecord.ts_unix_ms
            : typeof toolRecord?.created_unix_ms === "number"
              ? toolRecord.created_unix_ms
              : typeof toolRecord?.updated_unix_ms === "number"
                ? toolRecord.updated_unix_ms
                : ts + idx + 1;
        items.push({
          kind: "tool_record",
          ts: toolTs,
          run,
          runId: safeRunId,
          details,
          toolRecord,
        });
      });
    }
    items.sort((a, b) => a.ts - b.ts);
    return items;
  }, [dbMessages, dbRunDetailsById, dbRuns]);

  const lastAssistantMessage = React.useMemo(() => {
    let last: any = null;
    for (const message of dbMessages) {
      if (!message || typeof message !== "object") continue;
      if (message.role !== "assistant") continue;
      const ts = typeof message.created_unix_ms === "number" ? message.created_unix_ms : 0;
      if (!last || ts >= (typeof last.created_unix_ms === "number" ? last.created_unix_ms : 0)) {
        last = message;
      }
    }
    return last;
  }, [dbMessages]);

  const lastRun = React.useMemo(() => {
    let last: any = null;
    for (const run of dbRuns) {
      if (!run || typeof run !== "object") continue;
      const ts = typeof run.ts_unix_ms === "number" ? run.ts_unix_ms : 0;
      if (!last || ts >= (typeof last.ts_unix_ms === "number" ? last.ts_unix_ms : 0)) {
        last = run;
      }
    }
    return last;
  }, [dbRuns]);

  return (
    <div>
      <div className="mb-4">
        {lastAssistantMessage || lastRun ? (
          <div className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
            <div className="text-xs font-semibold text-white/70">Latest outcome</div>
            {lastAssistantMessage && typeof lastAssistantMessage.content === "string" ? (
              <details className="mt-2">
                <summary className="cursor-pointer text-[11px] text-white/60">Assistant response (collapsed)</summary>
                <div className="mt-2 text-sm text-white/90">
                  <Markdown text={String(lastAssistantMessage.content)} />
                </div>
              </details>
            ) : null}
            {lastRun ? (
              <details className="mt-2">
                <summary className="cursor-pointer text-[11px] text-white/60">Run info (collapsed)</summary>
                <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                  <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                    run
                  </span>
                  <span className="text-white/50">
                    {typeof lastRun.ts_unix_ms === "number" ? new Date(lastRun.ts_unix_ms).toLocaleString() : ""}
                  </span>
                  {typeof lastRun.ok === "number" ? (
                    <span className={lastRun.ok === 1 ? "text-emerald-300" : "text-rose-300"}>
                      {lastRun.ok === 1 ? "ok" : "error"}
                    </span>
                  ) : null}
                  {lastRun.error ? <span className="text-rose-200">{String(lastRun.error)}</span> : null}
                </div>
              </details>
            ) : null}
          </div>
        ) : null}

        <HistoryPanelTeamSection
          teamId={teamId}
          teamRunId={teamRunId}
          teamRunCreatedMs={teamRunCreatedMs}
          teamRunStatus={teamRunStatus}
          teamConversationItems={teamConversationItems}
          teamConversationWarnings={teamConversationWarnings}
          teamState={teamState}
          setShowSystemMessages={setShowSystemMessages}
        />

        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-sm font-semibold text-white/80">Conversation</div>
          <div className="text-[11px] text-white/50">
            {conversationItems.length > 0 ? `${conversationItems.length} items` : "no persisted history"}
          </div>
        </div>
        {conversationItems.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No persisted messages yet. Run a prompt to populate the chat history.
          </div>
        ) : (
          <div className="grid gap-3">
            {conversationItems.map((item, idx) => {
              if (item.kind === "message") {
                const message = item.message ?? {};
                const role = typeof message?.role === "string" ? message.role : "message";
                const content = typeof message?.content === "string" ? message.content : "";
                const ts = typeof message?.created_unix_ms === "number" ? message.created_unix_ms : 0;
                const when = ts ? new Date(ts).toLocaleString() : "";
                const truncated = message?.content_truncated ? true : false;
                const mmJson = typeof message?.mm_json === "string" ? message.mm_json : "";
                const mmBytes = typeof message?.mm_bytes === "number" ? message.mm_bytes : mmJson.length || 0;
                const isSystem = role === "system";
                const hasMeta = !!truncated || mmBytes > 0;
                return (
                  <div key={`msg:${ts || idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
                    <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                      <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                        {role}
                      </span>
                      {when ? <span>{when}</span> : null}
                      {truncated ? <span className="text-amber-200">truncated</span> : null}
                    </div>
                    {isSystem ? (
                      showSystemMessages ? (
                        <details className="mt-2">
                          <summary className="cursor-pointer text-xs text-white/60">System prompt (collapsed)</summary>
                          <div className="mt-2 text-sm text-white/90">
                            {content ? <Markdown text={content} /> : <span className="text-white/50">(no content)</span>}
                          </div>
                        </details>
                      ) : (
                        <div className="mt-2 text-xs text-white/40">System prompt hidden</div>
                      )
                    ) : (
                      <div className="mt-2 text-sm text-white/90">
                        {content ? <Markdown text={content} /> : <span className="text-white/50">(no content)</span>}
                      </div>
                    )}
                    {hasMeta ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">Message details</summary>
                        <div className="mt-2 text-[11px] text-white/60">
                          {mmBytes > 0 ? <div>mm bytes: {mmBytes}</div> : null}
                          {mmJson ? (
                            <details className="mt-2">
                              <summary className="cursor-pointer text-[11px] text-white/60">mm json</summary>
                              <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                                {formatJson(mmJson)}
                              </pre>
                            </details>
                          ) : null}
                        </div>
                      </details>
                    ) : null}
                  </div>
                );
              }

              if (item.kind === "tool_record") {
                const toolRecord = item.toolRecord ?? {};
                const toolName = typeof toolRecord?.tool_name === "string" ? toolRecord.tool_name : "tool";
                const argsJson = typeof toolRecord?.arguments_json === "string" ? toolRecord.arguments_json : "";
                let parsedArgs: any = null;
                try {
                  parsedArgs = argsJson ? JSON.parse(argsJson) : null;
                } catch {
                  parsedArgs = null;
                }
                const cmd = typeof parsedArgs?.cmd === "string" ? parsedArgs.cmd : "";
                const argvRaw = Array.isArray(parsedArgs?.argv) ? parsedArgs.argv : null;
                const argv = argvRaw ? argvRaw.map((x: any) => (typeof x === "string" ? x : "")).filter(Boolean).join(" ") : "";
                const command = cmd || argv;
                const resultText = typeof toolRecord?.result_text === "string" ? toolRecord.result_text : "";
                const resultForPrompt =
                  typeof toolRecord?.result_for_prompt_text === "string" ? toolRecord.result_for_prompt_text : "";
                const truncatedForPrompt = toolRecord?.result_truncated_for_prompt ? true : false;
                const shortCommand = command ? (command.length > 140 ? `${command.slice(0, 140)}…` : command) : "";
                return (
                  <div key={`tool:${item.ts || idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
                    <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                      <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                        tool
                      </span>
                      <span className="text-white/50">{toolName}</span>
                      {shortCommand ? <span className="text-white/40">· {shortCommand}</span> : null}
                    </div>
                    {command ? (
                      <div className="mt-2">
                        <div className="text-[11px] text-white/60">Command</div>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                          {command}
                        </pre>
                      </div>
                    ) : null}
                    {resultText ? (
                      <details className="mt-2" open>
                        <summary className="cursor-pointer text-[11px] text-white/60">Output</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {resultText}
                        </pre>
                      </details>
                    ) : null}
                    {resultForPrompt && resultForPrompt !== resultText ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">
                          Prompt-facing output{truncatedForPrompt ? " (truncated)" : ""}
                        </summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {resultForPrompt}
                        </pre>
                      </details>
                    ) : null}
                    {argsJson && !command ? (
                      <details className="mt-2" open>
                        <summary className="cursor-pointer text-[11px] text-white/60">Arguments</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {formatJson(argsJson)}
                        </pre>
                      </details>
                    ) : argsJson ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">Arguments (collapsed)</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {formatJson(argsJson)}
                        </pre>
                      </details>
                    ) : null}
                  </div>
                );
              }

              return null;
            })}
          </div>
        )}
      </div>

      <div className="mb-4">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div data-testid="history-artifact-heading" className="text-sm font-semibold text-white/80">
            {artifactCatalogMode === "direct" ? "Artifacts" : "Artifact references"}
          </div>
          <div className="text-[11px] text-white/50">
            {artifactCatalogMode === "unsupported"
              ? "connector-managed"
              : sessionArtifacts.length > 0
                ? `${sessionArtifacts.length} items`
                : "none"}
          </div>
        </div>
        {artifactCatalogMode === "unsupported" ? (
          <div
            data-testid="history-artifact-unsupported-note"
            className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-3 text-xs text-amber-100"
          >
            Broker connector mode does not expose a stable artifact catalog here. Use transcript, shell output, session
            events, and service metadata as the current result surfaces, and treat richer artifact browsing as a separate
            requirement.
          </div>
        ) : sessionArtifacts.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            {artifactCatalogMode === "broker_reference"
              ? "No broker artifact references are currently available from the session artifact surface."
              : "No artifacts captured yet."}
          </div>
        ) : (
          <div className="grid gap-2">
            {sessionArtifacts.slice(0, 20).map((row, idx) => {
              const data = row?.data ?? {};
              const artifact = data?.artifact ?? row?.artifact ?? row;
              const key =
                typeof artifact?.path === "string"
                  ? `artifact:${artifact.path}`
                  : typeof artifact?.resolved_path === "string"
                    ? `artifact:${artifact.resolved_path}`
                    : `artifact:${idx}`;
              return (
                <div key={key} className="rounded-md border border-white/10 bg-black/20 p-3">
                  <ArtifactView
                    baseUrl={props.effectiveBase}
                    yolo={props.yolo}
                    artifact={artifact}
                    allowAutoplay={props.allowAutoplay}
                    sessionId={props.sessionId}
                    client={props.client}
                    daemonAuth={props.daemonAuth}
                  />
                </div>
              );
            })}
          </div>
        )}
      </div>

      <HistoryPanelTechnicalSection
        entries={entries}
        showAllEntries={props.showAllEntries}
        setShowAllEntries={props.setShowAllEntries}
        showMessages={props.showMessages}
        setShowMessages={props.setShowMessages}
        historyExpandedByKey={props.historyExpandedByKey}
        setHistoryExpandedByKey={props.setHistoryExpandedByKey}
        effectiveBase={props.effectiveBase}
        sessionId={props.sessionId}
        yolo={props.yolo}
        client={props.client}
        daemonAuth={props.daemonAuth}
        showDebugInConversation={props.showDebugInConversation}
        allowAutoplay={props.allowAutoplay}
        allowClientRpcs={props.allowClientRpcs}
        allowClientEffects={props.allowClientEffects}
        allowUnsafePageEval={props.allowUnsafePageEval}
        sceneEntities={props.sceneEntities}
        onSceneApply={props.onSceneApply}
        onTraceIdClick={props.onTraceIdClick}
        showSystemMessages={showSystemMessages}
        setShowSystemMessages={setShowSystemMessages}
      />
    </div>
  );
}
