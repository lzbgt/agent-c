import React from "react";

import usePromptBarState from "./promptBar/usePromptBarState";
import PromptBarComposerBody from "./promptBar/PromptBarComposerBody";
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
      onKeyDown={(event) => event.stopPropagation()}
      onKeyUp={(event) => event.stopPropagation()}
      onKeyPress={(event) => event.stopPropagation()}
    >
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
            <div className="workspace-composer-actions">
              {props.queueCount ? <span className="text-xs text-white/50">{props.queueCount} queued</span> : null}
              {props.activeJobId ? <button className="workspace-button" onClick={() => void state.handleCancel()} type="button">Stop</button> : null}
              <button className="workspace-run rounded-lg px-4 py-2 text-sm disabled:opacity-40" onClick={state.onRun} disabled={props.runDisabled || (!props.prompt.trim() && !state.attachments.length)} type="button" data-testid="run">{props.runLabel}</button>
            </div>
          </>}
        />
        </div>
        {props.jobError || props.runError || props.resultError ? <div className="chat-send-error" role="alert">{String(props.jobError || props.runError || props.resultError).length <= 180 ? String(props.jobError || props.runError || props.resultError) : <>This request failed.<details><summary>Error details</summary><pre className="whitespace-pre-wrap break-words">{String(props.jobError || props.runError || props.resultError)}</pre></details></>}</div> : null}
        {props.jobNotice && !/^(queued|uploaded \d+ file)/.test(props.jobNotice) ? <div className="text-xs text-white/60" role="status">{props.jobNotice}</div> : null}

      </div>
    </div>
  );
});

export default PromptBar;
