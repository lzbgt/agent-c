import React from "react";

import usePromptBarState from "./promptBar/usePromptBarState";
import PromptBarComposerBody from "./promptBar/PromptBarComposerBody";
import PromptBarDetailsDrawer from "./promptBar/PromptBarDetailsDrawer";
import type { Attachment, PromptBarProps } from "./promptBar/promptBarTypes";

export type { Attachment } from "./promptBar/promptBarTypes";

const PromptBar = React.forwardRef<HTMLDivElement, PromptBarProps>(function PromptBar(props, ref) {
  const state = usePromptBarState(props);

  return (
    <div
      ref={ref}
      className="fixed bottom-0 left-0 right-0 z-30 border-t border-white/10 bg-slate-950/90 backdrop-blur"
      data-testid="promptbar-root"
    >
      <PromptBarDetailsDrawer
        activeJobId={props.activeJobId}
        attachments={state.attachments}
        chatTarget={state.chatTarget}
        drawerOpen={state.drawerOpen}
        jobError={props.jobError}
        jobNotice={props.jobNotice}
        jobProgressLabel={props.jobProgressLabel}
        jobStatus={props.jobStatus}
        onClearAttachments={() => state.setAttachments([])}
        onClose={() => state.setDrawerOpen(false)}
        onRemoveAttachment={state.removeAttachment}
        queueCount={props.queueCount}
        resultError={props.resultError}
        runError={props.runError}
        runWatchMode={props.runWatchMode}
        sessionId={props.sessionId}
        teamAction={state.teamAction}
        tools={props.tools}
      />

      <div className="mx-auto max-w-7xl px-3 py-3">
        <div className="flex min-w-0 items-center justify-between gap-2" data-testid="promptbar-summary-row">
          <div className="flex min-w-0 items-center gap-2 text-[11px] text-white/60">
            <button
              className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              type="button"
              data-testid="promptbar-toggle-collapse"
              onClick={() => state.setCollapsed((value) => !value)}
              aria-expanded={!state.collapsed}
              title={state.collapsed ? "Expand composer" : "Collapse composer"}
            >
              {state.collapsed ? "Expand" : "Collapse"}
              {state.collapsed && state.promptPreview ? <span className="max-w-[48vw] truncate text-white/50">{state.promptPreview}</span> : null}
              {state.attachments.length > 0 ? (
                <span className="text-white/50">
                  ({state.attachments.length} attachment{state.attachments.length === 1 ? "" : "s"})
                </span>
              ) : null}
            </button>

            <button
              className="inline-flex items-center gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
              type="button"
              data-testid="promptbar-toggle-details"
              onClick={() => state.setDrawerOpen((value) => !value)}
              aria-expanded={state.drawerOpen}
            >
              {state.drawerOpen ? "Hide details" : "Show details"}
              {props.jobError || props.runError || props.resultError ? <span className="text-rose-300">•</span> : null}
              {props.jobNotice ? <span className="text-amber-200">•</span> : null}
            </button>

            {state.teamAvailable ? (
              <div className="inline-flex items-center overflow-hidden rounded-md border border-white/10 bg-black/30 text-xs text-white/70">
                <button
                  className={`px-2 py-1 ${state.chatTarget === "session" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onChatTargetChange?.("session")}
                >
                  Session
                </button>
                <button
                  className={`px-2 py-1 ${state.chatTarget === "team" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onChatTargetChange?.("team")}
                >
                  Team {state.teamId ? `· ${state.teamId}` : ""}
                </button>
              </div>
            ) : null}

            {state.chatTarget === "team" ? (
              <div className="inline-flex items-center overflow-hidden rounded-md border border-white/10 bg-black/30 text-xs text-white/70">
                <button
                  className={`px-2 py-1 ${state.teamAction === "run" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("run")}
                >
                  Run
                </button>
                <button
                  className={`px-2 py-1 ${state.teamAction === "guidance" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("guidance")}
                >
                  Guidance
                </button>
                <button
                  className={`px-2 py-1 ${state.teamAction === "goal" ? "bg-indigo-500/20 text-indigo-100" : "hover:bg-black/40"}`}
                  type="button"
                  onClick={() => props.onTeamActionChange?.("goal")}
                >
                  Goal
                </button>
              </div>
            ) : null}
          </div>

          <div className="flex items-center gap-2">
            {props.activeJobId ? (
              <button
                className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15"
                onClick={() => void state.handleCancel()}
                type="button"
              >
                Cancel
              </button>
            ) : null}
            <button
              className="rounded-md bg-indigo-500 px-4 py-2 text-sm font-semibold text-white hover:bg-indigo-400 disabled:opacity-50"
              onClick={state.onRun}
              disabled={props.runDisabled}
              type="button"
              data-testid="run"
            >
              {props.runLabel}
            </button>
            {props.queueCount && props.queueCount > 0 ? (
              <span className="rounded-md border border-indigo-400/30 bg-indigo-500/10 px-2 py-1 text-xs text-indigo-100">
                {props.queueCount} queued
              </span>
            ) : null}
          </div>
        </div>

        <PromptBarComposerBody
          attachmentsCount={state.attachments.length}
          collapsed={state.collapsed}
          prompt={props.prompt}
          promptPlaceholder={state.promptPlaceholder}
          runDisabled={props.runDisabled}
          setJobNotice={props.setJobNotice}
          setPrompt={props.setPrompt}
          uploadBusy={state.uploadBusy}
          uploadsDisabledReason={state.uploadsDisabledReason}
          uploadsEnabled={state.uploadsEnabled}
          onRun={state.onRun}
          onUploadChange={state.handleUploadFiles}
        />
      </div>
    </div>
  );
});

export default PromptBar;
