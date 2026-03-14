import React from "react";

import type { PromptBarProps } from "./promptBarTypes";

type PromptBarComposerBodyProps = {
  attachmentsCount: number;
  collapsed: boolean;
  promptPlaceholder: string;
  uploadBusy: boolean;
  uploadsEnabled: boolean;
  uploadsDisabledReason: string;
  onRun: () => void;
  onUploadChange: (files: FileList | File[]) => Promise<void>;
} & Pick<PromptBarProps, "prompt" | "runDisabled" | "setJobNotice" | "setPrompt">;

export default function PromptBarComposerBody(props: PromptBarComposerBodyProps) {
  const fileInputRef = React.useRef<HTMLInputElement | null>(null);
  if (props.collapsed) return null;
  return (
    <>
      <textarea
        className="mt-2 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm leading-relaxed shadow-inner max-h-[30vh] overflow-y-auto"
        data-testid="prompt"
        rows={3}
        value={props.prompt}
        placeholder={props.promptPlaceholder}
        onChange={(event) => props.setPrompt(event.target.value)}
        onKeyDown={(event) => {
          if (props.runDisabled) return;
          if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
            event.preventDefault();
            props.onRun();
          }
        }}
      />

      <div className="mt-2 flex flex-wrap items-center gap-2" data-testid="promptbar-composer-body">
        <input
          ref={fileInputRef}
          type="file"
          multiple
          className="sr-only"
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
          className={`inline-flex cursor-pointer items-center gap-2 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 ${
            props.uploadBusy || !props.uploadsEnabled ? "opacity-50 pointer-events-none" : ""
          }`}
          disabled={props.uploadBusy || !props.uploadsEnabled}
          onClick={() => fileInputRef.current?.click()}
        >
          {props.uploadBusy ? "Uploading…" : "Attach files"}
        </button>
        {!props.uploadsEnabled ? (
          <div className="text-xs text-rose-200">{props.uploadsDisabledReason || "Uploads disabled by daemon caps."}</div>
        ) : null}

        {props.attachmentsCount > 0 ? (
          <div className="text-xs text-white/60" data-testid="promptbar-attachment-count">
            Staged: <span className="text-white/80">{props.attachmentsCount}</span>
          </div>
        ) : (
          <div className="text-xs text-white/50" data-testid="promptbar-attachment-count">
            No staged attachments
          </div>
        )}
      </div>
    </>
  );
}
