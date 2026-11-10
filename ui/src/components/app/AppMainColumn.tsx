import React from "react";
import type { ApiAuth } from "../../api";
import HistoryPanel from "../HistoryPanel";
import SceneView, { type SceneEntity } from "../SceneView";
import BrokerTeamConsole from "../broker/BrokerTeamConsole";
import type { SceneEntityMutationOp } from "../scene/sceneViewTypes";
import TeamHubCard from "./TeamHubCard";

type AppMainColumnProps = {
  sceneCollapsed: boolean;
  setSceneCollapsed: React.Dispatch<React.SetStateAction<boolean>>;
  effectiveBase: string;
  yolo: boolean;
  allowAutoplay: boolean;
  client: { id: string; kind: string; instance_id: string };
  daemonAuth: ApiAuth;
  sessionId: string;
  sceneEntities: SceneEntity[];
  onSceneApply: (ops: SceneEntityMutationOp[]) => void;
  brokerChatAvailable: boolean;
  teamHubProps: React.ComponentProps<typeof TeamHubCard>;
  inlineTeamSetupOpen: boolean;
  setInlineTeamSetupOpen: React.Dispatch<React.SetStateAction<boolean>>;
  advancedPage: string;
  inlineTeamConsoleProps: React.ComponentProps<typeof BrokerTeamConsole>;
  historyPanelProps: React.ComponentProps<typeof HistoryPanel>;
};

export default function AppMainColumn(props: AppMainColumnProps) {
  const history = props.historyPanelProps;
  const hasHistory = Boolean(history.activeJobId) || (history.dbMessages || []).some((message) => (message.role === "user" || message.role === "assistant") && (message.content || message.mm_json)) || [history.entries, history.dbRuns, history.sessionArtifacts, history.teamConversationItems].some((items) => items && items.length > 0);
  const hasScene = props.sceneEntities.length > 0;
  const hasDiagnostics = history.artifactCatalogMode === "unsupported";
  return (
    <section className="min-w-0" aria-label="Conversation workspace">
      {hasHistory ? <div className="mb-4">
        <HistoryPanel {...props.historyPanelProps} />
      </div> : null}
      {hasScene || !hasHistory ? (
      <div
        className={`workspace-surface workspace-scene p-4 ${
          hasScene && props.sceneCollapsed ? "" : "flex min-h-[280px] h-[min(48vh,520px)] flex-col"
        }`}
      >
        {hasScene ? (
        <div className="flex items-center justify-between gap-2">
          <div className="text-sm font-semibold text-white/80">Results</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => props.setSceneCollapsed((prev) => !prev)}
          >
            {props.sceneCollapsed ? "Show results" : "Collapse"}
          </button>
        </div>
        ) : null}
        {hasScene && props.sceneCollapsed ? (
          <div className="mt-2 text-xs text-white/50">Results are collapsed.</div>
        ) : (
          <div className="min-h-0 flex-1">
            <SceneView
              baseUrl={props.effectiveBase}
              yolo={props.yolo}
              allowAutoplay={props.allowAutoplay}
              client={props.client}
              daemonAuth={props.daemonAuth}
              sessionId={props.sessionId}
              entities={props.sceneEntities}
              className="h-full"
            />
          </div>
        )}
      </div>
      ) : null}

      {!hasHistory && (hasScene || hasDiagnostics) ? <HistoryPanel {...props.historyPanelProps} /> : null}

      {props.brokerChatAvailable ? <details className="conversation-details"><summary>Team setup</summary><TeamHubCard {...props.teamHubProps} /></details> : null}

      {props.brokerChatAvailable && props.inlineTeamSetupOpen && props.advancedPage !== "broker" ? (
        <details
          className="mt-3 rounded-lg border border-white/10 bg-black/20 p-3"
          open={props.inlineTeamSetupOpen}
          onToggle={(event) => props.setInlineTeamSetupOpen((event.currentTarget as HTMLDetailsElement).open)}
        >
          <summary className="cursor-pointer select-none text-xs font-semibold text-white/80">
            Inline team setup
          </summary>
          <div className="mt-2 text-[11px] text-white/60">
            Configure the team without leaving the chat. Attachments and prompts below are shared once you run.
          </div>
          <div className="mt-3">
            <BrokerTeamConsole {...props.inlineTeamConsoleProps} />
          </div>
        </details>
      ) : null}


    </section>
  );
}
