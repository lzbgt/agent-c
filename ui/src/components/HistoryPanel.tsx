import React from "react";
import type { ApiAuth } from "../api";
import type { ConversationViewProps } from "./conversation/conversationViewTypes";
import useLocalStorageState from "../hooks/useLocalStorageState";
import type { TeamConversationItem } from "../hooks/teamChatOrchestrationTypes";
import {
  buildPersistedConversationItems,
  findLastAssistantMessage,
  findLastRun,
  parseToolArgumentsJson,
  type DbMessageRow,
  type DbRunDetailRow,
  type DbRunSummaryRow,
  type HistoryEntry,
  type PersistedConversationItem,
  type SessionArtifactRow,
} from "../history/historyPanelData";
import type { SceneEntity } from "./SceneView";
import ArtifactView from "./ArtifactView";
import Markdown from "./Markdown";
import HistoryPanelTeamSection from "./history/HistoryPanelTeamSection";
import HistoryPanelTechnicalSection from "./history/HistoryPanelTechnicalSection";
import useHistoryPanelTeamState from "./history/useHistoryPanelTeamState";

export type HistoryPanelProps = {
  entries: HistoryEntry[];
  showAllEntries: boolean;
  setShowAllEntries: React.Dispatch<React.SetStateAction<boolean>>;
  showMessages: boolean;
  setShowMessages: React.Dispatch<React.SetStateAction<boolean>>;
  historyExpandedByKey: Record<string, boolean>;
  setHistoryExpandedByKey: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  dbMessages?: DbMessageRow[];
  dbRuns?: DbRunSummaryRow[];
  dbRunDetailsById?: Record<number, DbRunDetailRow>;
  sessionArtifacts?: SessionArtifactRow[];
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
  onSceneApply: ConversationViewProps["onSceneApply"];
  onTraceIdClick: (traceId: string) => void;
  teamConversationItems?: TeamConversationItem[];
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
  const entries = props.entries || [];
  const dbMessages = props.dbMessages || [];
  const dbRuns = props.dbRuns || [];
  const dbRunDetailsById = props.dbRunDetailsById || {};
  const sessionArtifacts = props.sessionArtifacts || [];
  const artifactCatalogMode = props.artifactCatalogMode || "direct";
  const teamConversationItems = props.teamConversationItems || [];
  const teamId = String(props.teamId || "").trim();
  const teamRunId = String(props.teamRunId || "").trim();
  const teamRunCreatedMs = typeof props.teamRunCreatedMs === "number" ? props.teamRunCreatedMs : 0;
  const teamRunStatus = String(props.teamRunStatus || "").trim();
  const teamConversationWarnings = props.teamConversationWarnings || [];

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

  const conversationItems = React.useMemo<PersistedConversationItem[]>(
    () => buildPersistedConversationItems(dbMessages, dbRuns, dbRunDetailsById),
    [dbMessages, dbRunDetailsById, dbRuns],
  );

  const lastAssistantMessage = React.useMemo(() => findLastAssistantMessage(dbMessages), [dbMessages]);
  const lastRun = React.useMemo(() => findLastRun(dbRuns), [dbRuns]);

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
                  {typeof lastRun.ok === "boolean" ? (
                    <span className={lastRun.ok ? "text-emerald-300" : "text-rose-300"}>
                      {lastRun.ok ? "ok" : "error"}
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
                const toolRecord = item.toolRecord;
                const toolName = toolRecord.tool_name || "tool";
                const argsJson = toolRecord.arguments_json || "";
                const parsedArgs = parseToolArgumentsJson(argsJson);
                const cmd = parsedArgs?.cmd || "";
                const argv = parsedArgs?.argv?.join(" ") || "";
                const command = cmd || argv;
                const resultText = toolRecord.result_text || "";
                const resultForPrompt = toolRecord.result_for_prompt_text || "";
                const truncatedForPrompt = toolRecord.result_truncated_for_prompt === true;
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
              const key =
                typeof row.path === "string"
                  ? `artifact:${row.path}`
                  : typeof row.resolved_path === "string"
                    ? `artifact:${row.resolved_path}`
                    : `artifact:${idx}`;
              return (
                <div key={key} className="rounded-md border border-white/10 bg-black/20 p-3">
                  <ArtifactView
                    baseUrl={props.effectiveBase}
                    yolo={props.yolo}
                    artifact={row.artifact}
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
