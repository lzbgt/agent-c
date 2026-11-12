import React from "react";
import type { ApiAuth } from "../api";
import { isUnknownRecord } from "../jsonUtils";
import ToolResultParsedView from "./toolResult/ToolResultParsedView";
import ToolResultPlainTextView from "./toolResult/ToolResultPlainTextView";
import { safeJsonParse } from "./toolResult/toolResultUtils";
import { useToolResultViewState } from "./toolResult/useToolResultViewState";

export default function ToolResultView({
  baseUrl,
  yolo,
  daemonAuth,
  sessionId,
  toolCallId,
  toolName,
  content,
}: {
  baseUrl: string;
  yolo: boolean;
  daemonAuth?: ApiAuth;
  sessionId?: string;
  toolCallId?: string;
  toolName?: string;
  content: string;
}) {
  const state = useToolResultViewState({ baseUrl, sessionId, toolCallId });
  const [copyNotice, setCopyNotice] = React.useState<string | null>(null);
  React.useEffect(() => setCopyNotice(null), [content]);

  const parsed = safeJsonParse(content);
  const data = isUnknownRecord(parsed) && isUnknownRecord(parsed.data) ? parsed.data : {};
  const name = toolName || (typeof data.tool === "string" ? data.tool : "");
  const hasTextOutput = typeof data.output === "string";
  // Copy captured text verbatim, never a model transcription.
  const capturedOutput = hasTextOutput ? data.output as string : content;
  const isJson = !hasTextOutput && isUnknownRecord(parsed);
  const capturedBytes = typeof data.captured_output_bytes === "number" && Number.isFinite(data.captured_output_bytes) && data.captured_output_bytes >= 0
    ? data.captured_output_bytes : null;
  const observedBytes = typeof data.observed_output_bytes === "number" && Number.isFinite(data.observed_output_bytes) && data.observed_output_bytes >= 0
    ? data.observed_output_bytes : null;
  const displayedBytes = React.useMemo(() => new TextEncoder().encode(capturedOutput).length, [capturedOutput]);
  const displayTruncated = data.output_display_truncated === true || data.output_truncated === true ||
    (hasTextOutput && capturedBytes !== null && displayedBytes < capturedBytes);
  const truncated = data.truncated === true || data.capture_truncated === true || displayTruncated ||
    (isUnknownRecord(parsed) && (parsed.truncated === true || parsed.output_truncated === true));
  const outputLabel = name === "fs_list" || name === "fs_find"
    ? `Directory listing (${name})`
    : name === "shell_exec" || name === "proc_exec"
      ? "Captured command output"
      : isJson ? "Structured tool result (JSON)" : "Captured tool output";

  const copyOutput = async () => {
    try {
      await navigator.clipboard.writeText(capturedOutput);
      setCopyNotice("Captured output copied.");
    } catch {
      setCopyNotice("Copy failed. Use Download output to save the captured text.");
    }
  };
  const downloadOutput = () => {
    const blob = new Blob([capturedOutput], { type: isJson ? "application/json;charset=utf-8" : "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${name.replace(/[^a-zA-Z0-9_-]/g, "_") || "tool-output"}.${isJson ? "json" : "txt"}`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  };

  return (
    <div>
      <div className="mb-2 flex flex-wrap items-center gap-2 text-xs text-white/70">
        <span>{outputLabel}</span>
        {capturedBytes !== null ? <span data-testid="tool-output-size">
          {capturedBytes} captured bytes{observedBytes !== null && observedBytes !== capturedBytes ? ` of ${observedBytes} observed` : ""}
          {displayTruncated ? `; ${displayedBytes} displayed` : ""}
        </span> : null}
        <button className="workspace-button ml-auto" data-testid="tool-output-copy" type="button" onClick={() => void copyOutput()}>Copy output</button>
        <button className="workspace-button" data-testid="tool-output-download" type="button" onClick={downloadOutput}>Download output</button>
      </div>
      {truncated ? <p className="mb-2 text-xs text-amber-200" role="status" data-testid="tool-output-truncation">
        {displayTruncated
          ? "This display is truncated. Copy and download include only the displayed excerpt."
          : "This result is truncated. Copy and download include only the captured output."}
      </p> : null}
      {copyNotice ? <p className="mb-2 text-xs text-white/70" role="status">{copyNotice}</p> : null}
      {isUnknownRecord(parsed) ? (
        <ToolResultParsedView
          content={content}
          daemonAuth={daemonAuth}
          parsed={parsed}
          sessionId={sessionId}
          state={state}
          yolo={yolo}
        />
      ) : (
        <ToolResultPlainTextView content={content} showFullOutput={state.showFullOutput} setShowFullOutput={state.setShowFullOutput} />
      )}
    </div>
  );
}
