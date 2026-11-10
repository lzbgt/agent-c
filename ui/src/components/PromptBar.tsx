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
      className="workspace-composer"
      hidden={props.hidden}
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

      <div className="workspace-composer-inner">
        <div className="chat-composer-box">
        <PromptBarComposerBody
          attachments={state.attachments}
          onRemoveAttachment={state.removeAttachment}
          prompt={props.prompt}
          promptPlaceholder={state.promptPlaceholder}
          runDisabled={props.runDisabled || (!props.prompt.trim() && !state.attachments.length)}
          setJobNotice={props.setJobNotice}
          setPrompt={props.setPrompt}
          uploadBusy={state.uploadBusy}
          uploadsDisabledReason={state.uploadsDisabledReason}
          uploadsEnabled={state.uploadsEnabled}
          onRun={state.onRun}
          onUploadChange={state.handleUploadFiles}
          controls={<>
            {state.teamAvailable ? <select className="chat-target" aria-label="Message target" value={state.chatTarget} onChange={(event) => props.onChatTargetChange?.(event.target.value as "session" | "team")}><option value="session">Conversation</option><option value="team">Team</option></select> : null}
            {state.chatTarget === "team" ? <select className="chat-target" aria-label="Team message type" value={state.teamAction} onChange={(event) => props.onTeamActionChange?.(event.target.value as "run" | "guidance" | "goal")}><option value="run">Task</option><option value="guidance">Guidance</option><option value="goal">Goal</option></select> : null}
            <button className="chat-more" type="button" aria-label="Message details" title="Message details" data-testid="promptbar-toggle-details" onClick={() => state.setDrawerOpen((value) => !value)} aria-expanded={state.drawerOpen}>···</button>
            <div className="workspace-composer-actions">
              {props.queueCount ? <span className="text-xs text-white/50">{props.queueCount} queued</span> : null}
              {props.activeJobId ? <button className="workspace-button" onClick={() => void state.handleCancel()} type="button">Stop</button> : null}
              <button className="workspace-run rounded-lg px-4 py-2 text-sm disabled:opacity-40" onClick={state.onRun} disabled={props.runDisabled || (!props.prompt.trim() && !state.attachments.length)} type="button" data-testid="run">{props.runLabel}</button>
            </div>
          </>}
        />
        </div>
        {props.jobError || props.runError || props.resultError ? <div className="chat-send-error" role="alert">{String(props.jobError || props.runError || props.resultError).slice(0, 180)}<button type="button" onClick={() => state.setDrawerOpen(true)}>Details</button></div> : null}
        {props.jobNotice && !/^(queued|uploaded \d+ file)/.test(props.jobNotice) ? <div className="text-xs text-white/60" role="status">{props.jobNotice}</div> : null}

      </div>
    </div>
  );
});

export default PromptBar;
