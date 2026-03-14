import React from "react";

import type { AgentEvent } from "../../api";
import { parseAssistantMultimodal, renderAssistantMultimodal } from "../AssistantMultimodal";
import ArtifactView from "../ArtifactView";
import ConversationCard from "../ConversationCard";
import LlmDebugView from "../LlmDebugView";
import Markdown from "../Markdown";
import ToolResultView from "../ToolResultView";
import ConversationUiActionCard from "./ConversationUiActionCard";
import { normalizeEventData, prettyJsonOrRaw, safeJsonParse } from "./utils";
import type { ConversationToolCallSummaryById, ConversationViewProps } from "./conversationViewTypes";

type ConversationEventListProps = Pick<
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
  | "prompt"
  | "reverseOrder"
  | "sceneEntities"
  | "sessionId"
  | "showDebugEvents"
  | "yolo"
> & {
  ackError: string | null;
  ackedKeys: Record<string, number>;
  markAckedKey: (key: string) => void;
  postClientEvent: (type: string, payload: any) => Promise<void>;
  rpcRuntime: React.ComponentProps<typeof ConversationUiActionCard>["runtime"];
  setAckError: (msg: string | null) => void;
  toolCallSummaryById: ConversationToolCallSummaryById;
};

const Card = ConversationCard;

export default function ConversationEventList(props: ConversationEventListProps) {
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
    prompt,
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
  let streamedAssistant = "";
  let sawFinalAssistant = false;
  let sawToolOrAssistant = false;
  let lastHeartbeat: any = null;

  if (prompt.trim().length > 0) {
    items.push(
      <Card key="user" title="User">
        <Markdown text={prompt} />
      </Card>,
    );
  }

  events.forEach((ev: AgentEvent, idx: number) => {
    const type = ev.type;
    const data: any = normalizeEventData(ev.data);

    if (type === "assistant_message") {
      sawFinalAssistant = true;
      sawToolOrAssistant = true;
      const mm = parseAssistantMultimodal(data);
      items.push(
        <Card key={`a-${idx}`} title="Assistant">
          <Markdown text={String(data.assistant_content ?? "")} />
          {renderAssistantMultimodal(mm)}
        </Card>,
      );
      return;
    }

    if (type === "assistant_delta") {
      const delta = typeof data.delta === "string" ? data.delta : "";
      if (delta) streamedAssistant += delta;
      if (delta) sawToolOrAssistant = true;
      return;
    }

    if (type === "tool_call") {
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      const toolCallId = typeof data?.tool_call_id === "string" ? String(data.tool_call_id) : "";
      const args =
        typeof data.arguments_json === "string"
          ? data.arguments_json
          : data.arguments && typeof data.arguments === "object"
            ? JSON.stringify(data.arguments)
            : typeof data.args === "object" && data.args
              ? JSON.stringify(data.args)
              : "";
      const parsedArgs = args ? safeJsonParse(args) : null;
      const toolCallSummary = (() => {
        if (!parsedArgs || typeof parsedArgs !== "object") return null;
        if (name === "shell_exec") {
          const cmd = typeof (parsedArgs as any)?.cmd === "string" ? String((parsedArgs as any).cmd) : "";
          return cmd ? { label: "cmd", value: cmd } : null;
        }
        if (name === "proc_exec") {
          const argvRaw = (parsedArgs as any)?.argv;
          const argv = Array.isArray(argvRaw) ? argvRaw.map((value) => (typeof value === "string" ? value : "")).filter(Boolean) : [];
          return argv.length > 0 ? { label: "argv", value: argv.join(" ") } : null;
        }
        return null;
      })();
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
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      const toolCallId = typeof data?.tool_call_id === "string" ? String(data.tool_call_id) : "";
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
      if (data.summary && typeof data.summary === "object") {
        let envelope = "";
        try {
          envelope = JSON.stringify({ ok: (data.summary as any)?.ok ?? true, data: { ...(data.summary as any), tool: name } });
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
      sawToolOrAssistant = true;
      const artifact = { ...(data?.artifact ?? {}), tool_call_id: String(data?.tool_call_id ?? "") };
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
      sawToolOrAssistant = true;
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
      lastHeartbeat = data ?? {};
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
      if (type === "llm_request" || type === "llm_response" || type === "start" || type === "end" || type === "done") {
        if (type === "llm_request") {
          items.push(
            <Card key={`dbg-${type}-${idx}`} title="LLM request">
              <LlmDebugView kind="request" data={data} />
            </Card>,
          );
          return;
        }
        if (type === "llm_response") {
          items.push(
            <Card key={`dbg-${type}-${idx}`} title="LLM response">
              <LlmDebugView kind="response" data={data} />
            </Card>,
          );
          return;
        }
        items.push(
          <Card key={`dbg-${type}-${idx}`} title={`Debug: ${type}`}>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
    }

    if (type === "llm_request" || type === "llm_response") return;
    if (type === "start" || type === "end" || type === "done" || type === "retry" || type === "compaction") return;
    if (type === "error") {
      items.push(
        <Card key={`e-${idx}`} title="Error">
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
            {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
          </pre>
        </Card>,
      );
    }
  });

  if (!sawToolOrAssistant && events.length > 0) {
    const heartbeatJson = lastHeartbeat && typeof lastHeartbeat === "object" ? JSON.stringify(lastHeartbeat, null, 2) : "";
    items.push(
      <Card key="working" title="Working…">
        <div className="text-xs text-white/70">
          Waiting for assistant output / tool calls.
          {heartbeatJson ? (
            <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-[11px] leading-relaxed text-white/80">
              {heartbeatJson}
            </pre>
          ) : null}
        </div>
      </Card>,
    );
  }

  if (!sawFinalAssistant && streamedAssistant.trim().length > 0) {
    items.push(
      <Card key="assistant-stream" title="Assistant (streaming)">
        <Markdown text={streamedAssistant} />
      </Card>,
    );
  }

  const displayItems = reverseOrder ? [...items].reverse() : items;
  return (
    <div className="grid gap-3" data-testid="conversation-view">
      {displayItems}
    </div>
  );
}
