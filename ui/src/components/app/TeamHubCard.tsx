import React from "react";

export type TeamHubQueueEntry = {
  prompt: string;
  attachments: unknown[];
  queued_unix_ms: number;
  action: "run" | "guidance" | "goal";
};

export type TeamHubActivity = {
  key: string;
  label: string;
  preview: string;
  ts?: number;
};

type TeamHubCardProps = {
  selectedTeamId: string;
  latestTeamRunId: string;
  teamStatus: string;
  inlineTeamSetupOpen: boolean;
  onToggleInlineSetup: () => void;
  onViewChat: () => void;
  onOpenFullSetup: () => void;
  onOpenMembers: () => void;
  onOpenRuns: () => void;
  onRunTeam: () => void;
  onSendGuidance: () => void;
  onSetGoal: () => void;
  teamConversationUsingCache: boolean;
  teamConversationCacheUpdatedMs?: number;
  teamQueueCount: number;
  teamQueue: TeamHubQueueEntry[];
  teamQueueNeedsRun: boolean;
  onClearQueue: () => void;
  onStartQueuedRun: () => void;
  recentActivity: TeamHubActivity[];
};

export default function TeamHubCard(props: TeamHubCardProps) {
  return (
    <div className="mt-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <div className="text-xs font-semibold text-white/80">Team hub</div>
          <div className="text-[11px] text-white/60">
            Team <span className="text-white/80">{props.selectedTeamId}</span>
            {props.latestTeamRunId ? (
              <>
                {" "}
                · run <span className="text-white/80">{props.latestTeamRunId}</span>
              </>
            ) : null}
            {props.teamStatus ? (
              <>
                {" "}
                · <span className="text-white/50">{props.teamStatus}</span>
              </>
            ) : null}
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onToggleInlineSetup}
          >
            {props.inlineTeamSetupOpen ? "Hide setup" : "Quick setup"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onViewChat}
          >
            View chat
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onOpenFullSetup}
          >
            Full setup
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onOpenMembers}
          >
            Members
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onOpenRuns}
          >
            Runs
          </button>
          <button
            className="rounded-md border border-indigo-400/30 bg-indigo-500/20 px-3 py-1 text-[11px] text-indigo-100 hover:bg-indigo-500/30"
            type="button"
            onClick={props.onRunTeam}
          >
            Run team
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onSendGuidance}
          >
            Send guidance
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.onSetGoal}
          >
            Set goal
          </button>
        </div>
      </div>
      <div className="mt-2 text-[11px] text-white/50">
        Prompts and attachments below are shared with all team members. Use Guidance for mid-run updates.
      </div>
      {props.teamConversationUsingCache ? (
        <div className="mt-2 text-[11px] text-amber-100">
          Showing cached team history
          {props.teamConversationCacheUpdatedMs
            ? ` (last updated ${new Date(props.teamConversationCacheUpdatedMs).toLocaleString()})`
            : ""}{" "}
          because live history is unavailable.
        </div>
      ) : null}
      {props.teamQueueCount > 0 ? (
        <div className="mt-2 rounded-md border border-indigo-400/20 bg-indigo-500/10 px-2 py-2 text-[11px] text-indigo-100">
          <div className="flex items-center justify-between gap-2">
            <div className="font-semibold text-indigo-100">Queued team actions: {props.teamQueueCount}</div>
            <button
              className="rounded-md border border-indigo-400/30 bg-indigo-500/20 px-2 py-0.5 text-[11px] text-indigo-100 hover:bg-indigo-500/30"
              type="button"
              onClick={props.onClearQueue}
            >
              Clear queue
            </button>
          </div>
          {props.teamQueueNeedsRun ? (
            <div className="mt-1 flex flex-wrap items-center gap-2 text-amber-100">
              <span className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-[10px] uppercase tracking-wide">
                needs run
              </span>
              <span className="text-[11px] text-amber-100/90">Start a run to release queued guidance/goal.</span>
              <button
                className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-[11px] text-amber-100 hover:bg-amber-500/20"
                type="button"
                onClick={props.onStartQueuedRun}
              >
                Start run
              </button>
            </div>
          ) : null}
          <div className="mt-1 grid gap-1 text-indigo-100/80">
            {props.teamQueue.slice(0, 3).map((entry, idx) => {
              const snippet = entry.prompt.trim();
              const preview = snippet.length > 80 ? `${snippet.slice(0, 80)}…` : snippet || "(no prompt)";
              const attachmentCount = Array.isArray(entry.attachments) ? entry.attachments.length : 0;
              const waitingForRun = entry.action !== "run" && !props.latestTeamRunId;
              return (
                <div key={`team-queue-${entry.queued_unix_ms}-${idx}`} className="flex items-center gap-2">
                  <span className="rounded-md border border-indigo-400/30 bg-indigo-500/20 px-2 py-0.5 text-[10px] uppercase tracking-wide text-indigo-100">
                    {entry.action}
                  </span>
                  <span className="text-indigo-100/80">{preview}</span>
                  {attachmentCount > 0 ? (
                    <span className="rounded-md border border-indigo-400/30 bg-indigo-500/20 px-2 py-0.5 text-[10px] text-indigo-100">
                      +{attachmentCount} file{attachmentCount === 1 ? "" : "s"}
                    </span>
                  ) : null}
                  {waitingForRun ? (
                    <span className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-[10px] text-amber-100">
                      waiting for run
                    </span>
                  ) : null}
                </div>
              );
            })}
            {props.teamQueueCount > 3 ? (
              <div className="text-[10px] text-indigo-100/70">
                +{props.teamQueueCount - 3} more queued
              </div>
            ) : null}
          </div>
        </div>
      ) : null}
      {props.recentActivity.length > 0 ? (
        <div className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/70">
          <div className="flex items-center justify-between gap-2">
            <div className="font-semibold text-white/70">Recent activity</div>
            <div className="text-[10px] text-white/40">
              {props.recentActivity[0]?.ts
                ? new Date(props.recentActivity[0].ts).toLocaleTimeString()
                : ""}
            </div>
          </div>
          <div className="mt-1 grid gap-1">
            {props.recentActivity.map((entry) => (
              <div key={entry.key} className="flex flex-wrap items-baseline gap-2">
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] uppercase tracking-wide text-white/60">
                  {entry.label}
                </span>
                <span className="text-white/80">{entry.preview}</span>
              </div>
            ))}
          </div>
        </div>
      ) : null}
    </div>
  );
}
