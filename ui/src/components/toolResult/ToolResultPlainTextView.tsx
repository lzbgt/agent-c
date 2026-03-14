import React from "react";
import { firstNLines } from "./toolResultUtils";

type ToolResultPlainTextViewProps = {
  content: string;
  showFullOutput: boolean;
  setShowFullOutput: (next: boolean) => void;
};

export default function ToolResultPlainTextView(props: ToolResultPlainTextViewProps) {
  const { content, setShowFullOutput, showFullOutput } = props;
  const peek = firstNLines(content, 5);
  const isLong = peek.totalLines > 5 || content.length > 2000;

  return (
    <div>
      {isLong ? (
        <div className="mb-2">
          <div className="mb-1 text-[11px] text-white/60">
            Peek (first 5 lines){peek.totalLines > 5 ? ` of ${peek.totalLines}` : ""}:
          </div>
          <pre
            className="cursor-pointer overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90 hover:bg-black/35"
            title="Click to expand/collapse output"
            onClick={() => setShowFullOutput(!showFullOutput)}
          >
            {peek.head}
          </pre>
        </div>
      ) : null}

      {!isLong || showFullOutput ? (
        <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
          {content}
        </pre>
      ) : (
        <div className="text-[11px] text-white/50">Output collapsed. Expand to view full content.</div>
      )}

      {isLong ? (
        <button
          className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
          onClick={() => setShowFullOutput(!showFullOutput)}
          type="button"
        >
          {showFullOutput ? "Collapse output" : "Expand output"}
        </button>
      ) : null}
    </div>
  );
}
