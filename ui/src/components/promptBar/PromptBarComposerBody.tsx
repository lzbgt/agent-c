import React from "react";

import type { Attachment, PromptBarProps } from "./promptBarTypes";
import WorkspaceIcon from "../app/WorkspaceIcon";

type PromptBarComposerBodyProps = {
  attachments: Attachment[];
  onRemoveAttachment: (path: string) => void;
  controls: React.ReactNode;
  promptPlaceholder: string;
  uploadBusy: boolean;
  uploadsEnabled: boolean;
  uploadsDisabledReason: string;
  onRun: () => void;
  onUploadChange: (files: FileList | File[]) => Promise<void>;
} & Pick<PromptBarProps, "prompt" | "runDisabled" | "setJobNotice" | "setPrompt">;

export default function PromptBarComposerBody(props: PromptBarComposerBodyProps) {
  const fileInputRef = React.useRef<HTMLInputElement | null>(null);
  return (
    <>
      <textarea
        className="w-full resize-none border-0 bg-transparent px-1 py-2 text-sm leading-relaxed max-h-[30vh] overflow-y-auto"
        data-testid="prompt"
        aria-label="Message your agent"
        rows={2}
        value={props.prompt}
        placeholder={props.promptPlaceholder}
        onChange={(event) => props.setPrompt(event.target.value)}
        onKeyDown={(event) => {
          if (props.runDisabled) return;
          if (!event.nativeEvent.isComposing && event.key === "Enter" && (event.ctrlKey || event.metaKey || (!event.shiftKey && window.matchMedia("(pointer: fine)").matches))) {
            event.preventDefault();
            props.onRun();
          }
        }}
      />

      {props.attachments.length ? <div className="chat-attachments" data-testid="promptbar-attachment-count">
        {props.attachments.map((file) => <span key={file.path}>{file.name || file.path}<button type="button" onClick={() => props.onRemoveAttachment(file.path)} aria-label={`Remove ${file.name || file.path}`}>×</button></span>)}
      </div> : null}
      <div className="chat-composer-footer" data-testid="promptbar-composer-body">
        <input
          ref={fileInputRef}
          type="file"
          multiple
          className="sr-only"
          tabIndex={-1}
          data-testid="promptbar-file-input"
          disabled={!props.uploadsEnabled || props.uploadBusy}
          onChange={async (event) => {
            if (!props.uploadsEnabled) {
              props.setJobNotice("uploads disabled by daemon caps");
              return;
            }
            const files = Array.from(event.target.files || []);
            event.currentTarget.value = "";
            await props.onUploadChange(files);
          }}
        />
        <button
          type="button"
          data-testid="promptbar-attach-button"
          className={`chat-attach inline-flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-xs text-white/70 hover:bg-white/5 ${
            props.uploadBusy || !props.uploadsEnabled ? "opacity-50 pointer-events-none" : ""
          }`}
          disabled={props.uploadBusy || !props.uploadsEnabled}
          aria-label={props.uploadBusy ? "Uploading files" : "Attach files"}
          title="Attach files"
          onClick={() => fileInputRef.current?.click()}
        >
          <WorkspaceIcon name="attach" />
          <span>{props.uploadBusy ? "Uploading…" : "Attach"}</span>
        </button>
        {!props.uploadsEnabled ? (
          <div className="text-xs text-rose-200">{props.uploadsDisabledReason || "Uploads disabled by daemon caps."}</div>
        ) : null}

        {props.controls}
      </div>
    </>
  );
}
