import React from "react";

import ConversationCard from "../ConversationCard";
import { parseConversationUiAction } from "./conversationData";
import { safeTrunc, tryParseUrl } from "./utils";
import type { ConversationUiActionCardProps } from "./conversationUiActionTypes";

const Card = ConversationCard;

export default function ConversationUiActionAckCard({
  data,
  idx,
  sessionId,
  ackedKeys,
  ackError,
  setAckError,
  markAckedKey,
  postClientEvent,
}: ConversationUiActionCardProps) {
  const parsed = parseConversationUiAction(data);
  const action = parsed.action;
  const atype = parsed.atype;
  const title = parsed.title;
  const toolCallId = parsed.toolCallId;

  if (atype === "request_client_state" || atype === "request_state") {
    const queryId = parsed.queryId;
    const canAck = typeof sessionId === "string" && sessionId.trim().length > 0 && !!queryId;
    const ackKey = toolCallId ? `client_state:${toolCallId}` : `client_state:${queryId}`;

    const gatherMediaSnapshot = (): Array<Record<string, unknown>> => {
      if (typeof document === "undefined") return [];
      const els = Array.from(document.querySelectorAll("audio,video")).slice(0, 20);
      return els.map((el) => {
        const isVideo = el.tagName.toLowerCase() === "video";
        const mediaElement = el as HTMLMediaElement;
        const dataset = mediaElement.dataset;
        const snapshot: Record<string, unknown> = {
          kind: isVideo ? "video" : "audio",
          paused: mediaElement.paused,
          ended: mediaElement.ended,
        };
        const src = mediaElement.currentSrc || mediaElement.src || "";
        if (src) {
          snapshot.src = safeTrunc(src, 300);
          const u = tryParseUrl(src);
          if (u && u.pathname.endsWith("/api/v1/file")) {
            const p = u.searchParams.get("path") || "";
            if (p) snapshot.path = safeTrunc(p, 200);
          }
        }
        if (typeof dataset.toolCallId === "string" && dataset.toolCallId.length > 0) snapshot.tool_call_id = dataset.toolCallId;
        if (typeof dataset.path === "string" && dataset.path.length > 0) snapshot.path = safeTrunc(dataset.path, 200);

        if (Number.isFinite(mediaElement.currentTime)) snapshot.current_time = mediaElement.currentTime;
        if (Number.isFinite(mediaElement.duration)) snapshot.duration = mediaElement.duration;
        return snapshot;
      });
    };

    return (
      <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
          Agent requested a client state snapshot.
        </div>
        <div className="mt-2 flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canAck || !!ackedKeys[ackKey]}
            title={!canAck ? "Select a session first" : toolCallId ? `tool_call_id=${toolCallId}` : ""}
            onClick={() => {
              setAckError(null);
              void (async () => {
                try {
                  await postClientEvent("client_state", {
                    query_id: queryId,
                    request_tool_call_id: toolCallId,
                    url: safeTrunc(String(window?.location?.href ?? ""), 400),
                    media: gatherMediaSnapshot(),
                  });
                  markAckedKey(ackKey);
                } catch (e) {
                  setAckError(String(e));
                }
              })();
            }}
          >
            {ackedKeys[ackKey] ? "Snapshot sent" : "Send snapshot"}
          </button>
          {ackError ? <div className="text-[11px] text-amber-200/80">snapshot failed: {ackError}</div> : null}
        </div>
      </Card>
    );
  }

  if (atype === "notify") {
    const msg = typeof action.message === "string" ? action.message : "";
    const ackKey = toolCallId ? `tool_call:${toolCallId}` : `notify:${title}:${msg}`;
    const canAck = typeof sessionId === "string" && sessionId.trim().length > 0;
    return (
      <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
          {msg || "(no message)"}
        </div>
        <div className="mt-2 flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canAck || !!ackedKeys[ackKey]}
            title={!canAck ? "Select a session first" : toolCallId ? `tool_call_id=${toolCallId}` : ""}
            onClick={() => {
              setAckError(null);
              void (async () => {
                try {
                  await postClientEvent("notification_ack", {
                    tool_call_id: toolCallId,
                    action_type: "notify",
                    title,
                    message: msg,
                  });
                  markAckedKey(ackKey);
                } catch (e) {
                  setAckError(String(e));
                }
              })();
            }}
          >
            {ackedKeys[ackKey] ? "Acknowledged" : "Acknowledge"}
          </button>
          {ackError ? <div className="text-[11px] text-amber-200/80">ack failed: {ackError}</div> : null}
        </div>
      </Card>
    );
  }

  return null;
}
