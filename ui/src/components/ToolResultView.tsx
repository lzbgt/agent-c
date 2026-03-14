import React from "react";
import type { ApiAuth } from "../api";
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
  content,
}: {
  baseUrl: string;
  yolo: boolean;
  daemonAuth?: ApiAuth;
  sessionId?: string;
  toolCallId?: string;
  content: string;
}) {
  const state = useToolResultViewState({
    baseUrl,
    sessionId,
    toolCallId,
  });

  const parsed = safeJsonParse(content);
  if (parsed && typeof parsed === "object") {
    return (
      <ToolResultParsedView
        content={content}
        daemonAuth={daemonAuth}
        parsed={parsed}
        sessionId={sessionId}
        state={state}
        yolo={yolo}
      />
    );
  }

  return <ToolResultPlainTextView content={content} showFullOutput={state.showFullOutput} setShowFullOutput={state.setShowFullOutput} />;
}
