import React from "react";

import type { AgentEvent } from "../../api";
import { executionEvent } from "../../history/executionEvidence";
import ArtifactView from "../ArtifactView";
import ConversationCard from "../ConversationCard";
import ToolResultView from "../ToolResultView";
import ConversationUiActionCard from "./ConversationUiActionCard";
import {
  buildToolCallPreviewFromArgs,
  extractToolCallArgumentsText,
  getNormalizedEventData,
} from "./conversationData";
import { prettyJsonOrRaw, safeObject } from "./utils";
import type { ConversationToolCallSummaryById, ConversationViewProps } from "./conversationViewTypes";

type ExecutionEventListProps = Pick<
  ConversationViewProps,
  | "allowAutoplay"
  | "allowClientEffects"
  | "allowClientRpcs"
  | "allowUnsafePageEval"
  | "baseUrl"
  | "client"
  | "daemonAuth"
  | "disableAutoClientRpcs"
  | "events"
  | "onSceneApply"
  | "reverseOrder"
  | "sceneEntities"
  | "sessionId"
  | "showDebugEvents"
  | "yolo"
> & {
  ackError: string | null;
  ackedKeys: Record<string, number>;
  markAckedKey: (key: string) => void;
  postClientEvent: (type: string, payload: unknown) => Promise<void>;
  rpcRuntime: React.ComponentProps<typeof ConversationUiActionCard>["runtime"];
  setAckError: (msg: string | null) => void;
  toolCallSummaryById: ConversationToolCallSummaryById;
};

const Card = ConversationCard;

export default function ExecutionEventList(props: ExecutionEventListProps) {
  const {
    ackError,
    ackedKeys,
    allowAutoplay,
    allowClientEffects,
    allowClientRpcs,
    allowUnsafePageEval,
    baseUrl,
    client,
    daemonAuth,
    disableAutoClientRpcs,
    events,
    markAckedKey,
    onSceneApply,
    postClientEvent,
    reverseOrder,
    rpcRuntime,
    sceneEntities,
    sessionId,
    setAckError,
    showDebugEvents,
    toolCallSummaryById,
    yolo,
  } = props;

  const items: Array<React.ReactNode> = [];
  events.forEach((original: AgentEvent, idx: number) => {
    const ev = executionEvent(original);
    if (!ev) return;
    const type = ev.type;
    const rawData = getNormalizedEventData(ev.data);
    const data = safeObject(rawData);

    if (type === "tool_call") {
      const name = String(data.tool_name ?? "");
      const toolCallId = typeof data.tool_call_id === "string" ? data.tool_call_id : "";
      const args = extractToolCallArgumentsText(data);
      const toolCallSummary = buildToolCallPreviewFromArgs(name, args);
      const summaryFromResult = toolCallId && toolCallSummaryById[toolCallId] ? toolCallSummaryById[toolCallId] : null;
      const inferredSummary = (() => {
        if (toolCallSummary) return toolCallSummary;
        if (!summaryFromResult) return null;
        if (name === "shell_exec" && summaryFromResult.cmd) return { label: "cmd", value: summaryFromResult.cmd };
        if (name === "proc_exec" && summaryFromResult.argv) return { label: "argv", value: summaryFromResult.argv };
        return null;
      })();
      items.push(
        <Card key={`tc-${idx}`} title={`Tool call: ${name || "(unknown)"}`}>
          {inferredSummary ? (
            <div className="mb-2">
              <div className="mb-1 text-[11px] font-semibold text-white/60">Command</div>
              <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                {inferredSummary.value}
              </pre>
            </div>
          ) : null}
          <details>
            <summary className="cursor-pointer text-[11px] text-white/60">Arguments (collapsed)</summary>
            <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {args ? prettyJsonOrRaw(args) : inferredSummary ? "(arguments omitted; see command above)" : "(enable verbose to capture arguments)"}
            </pre>
          </details>
        </Card>,
      );
      return;
    }

    if (type === "tool_result") {
      const name = String(data.tool_name ?? "");
      const toolCallId = typeof data.tool_call_id === "string" ? data.tool_call_id : "";
      const summaryFromResult = toolCallId && toolCallSummaryById[toolCallId] ? toolCallSummaryById[toolCallId] : null;
      const commandLine = summaryFromResult?.cmd ? summaryFromResult.cmd : summaryFromResult?.argv ? summaryFromResult.argv : "";
      if (typeof data.content === "string") {
        items.push(
          <Card key={`tr-${idx}`} title={`Tool output: ${name || "(unknown)"}`}>
            {commandLine ? (
              <div className="mb-2">
                <div className="mb-1 text-[11px] font-semibold text-white/60">Command</div>
                <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                  {commandLine}
                </pre>
              </div>
            ) : null}
            <ToolResultView
              baseUrl={baseUrl}
              yolo={yolo}
              daemonAuth={daemonAuth}
              sessionId={sessionId}
              toolCallId={toolCallId}
              content={data.content}
            />
          </Card>,
        );
        return;
      }
      const summaryRecord = safeObject(data.summary);
      if (Object.keys(summaryRecord).length > 0) {
        let envelope = "";
        try {
          envelope = JSON.stringify({ ok: summaryRecord.ok ?? true, data: { ...summaryRecord, tool: name } });
        } catch {
          envelope = JSON.stringify({ ok: true, data: { tool: name } });
        }
        items.push(
          <Card key={`tr-${idx}`} title={`Tool output: ${name || "(unknown)"}`}>
            {commandLine ? (
              <div className="mb-2">
                <div className="mb-1 text-[11px] font-semibold text-white/60">Command</div>
                <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                  {commandLine}
                </pre>
              </div>
            ) : null}
            <ToolResultView
              baseUrl={baseUrl}
              yolo={yolo}
              daemonAuth={daemonAuth}
              sessionId={sessionId}
              toolCallId={toolCallId}
              content={envelope}
            />
          </Card>,
        );
        return;
      }
      items.push(
        <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {data.summary ? JSON.stringify(data.summary, null, 2) : "(enable verbose to capture tool output)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "artifact") {
      const artifact: Record<string, unknown> & { tool_call_id: string } = {
        ...safeObject(data.artifact),
        tool_call_id: String(data.tool_call_id ?? ""),
      };
      const title = String(artifact?.title ?? artifact?.path ?? "artifact");
      items.push(
        <Card key={`af-${idx}`} title={`Artifact: ${title}`}>
          <ArtifactView
            baseUrl={baseUrl}
            yolo={yolo}
            artifact={artifact}
            allowAutoplay={allowAutoplay}
            sessionId={sessionId}
            client={client}
            daemonAuth={daemonAuth}
          />
        </Card>,
      );
      return;
    }

    if (type === "ui_action") {
      items.push(
        <ConversationUiActionCard
          key={`ua-${idx}`}
          baseUrl={baseUrl}
          yolo={yolo}
          sessionId={sessionId}
          daemonAuth={daemonAuth}
          allowClientRpcs={allowClientRpcs}
          allowClientEffects={allowClientEffects}
          allowUnsafePageEval={allowUnsafePageEval}
          disableAutoClientRpcs={disableAutoClientRpcs}
          data={data}
          idx={idx}
          ackedKeys={ackedKeys}
          ackError={ackError}
          setAckError={setAckError}
          markAckedKey={markAckedKey}
          postClientEvent={postClientEvent}
          runtime={rpcRuntime}
          sceneEntities={sceneEntities}
          onSceneApply={onSceneApply}
        />,
      );
      return;
    }

    if (type === "heartbeat") {
      return;
    }

    if (showDebugEvents) {
      if (type === "retry") {
        items.push(
          <Card key={`rt-${idx}`} title="Retry">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancel_requested") {
        items.push(
          <Card key={`cr-${idx}`} title="Cancel requested">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancelled") {
        items.push(
          <Card key={`cx-${idx}`} title="Cancelled">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "compaction") {
        items.push(
          <Card key={`cp-${idx}`} title="Compaction">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "start" || type === "end" || type === "done") {
        const metadata = Object.fromEntries(["ok", "status", "model", "tools", "trace_id", "job_id", "elapsed_ms", "usage", "finish_reason"].filter((key) => data[key] !== undefined).map((key) => [key, data[key]]));
        items.push(<Card key={`dbg-${type}-${idx}`} title={`Debug: ${type}`}><pre className="overflow-auto whitespace-pre-wrap">{JSON.stringify(metadata, null, 2)}</pre></Card>);
        return;
      }
    }

    if (type === "llm_request" || type === "llm_response") return;
    if (type === "start" || type === "end" || type === "done" || type === "retry" || type === "compaction") return;
    if (type === "error") {
      items.push(
        <Card key={`e-${idx}`} title="Error">
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
            {typeof rawData === "string" ? rawData : JSON.stringify(rawData, null, 2)}
          </pre>
        </Card>,
      );
    }
  });

  const displayItems = reverseOrder ? [...items].reverse() : items;
  return (
    <div className="grid gap-3" data-testid="execution-activity">
      {displayItems}
    </div>
  );
}
