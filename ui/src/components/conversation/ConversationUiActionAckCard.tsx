import React from "react";

import ConversationCard from "../ConversationCard";
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
  const action = data?.action ?? {};
  const atype = String(action?.type ?? "");
  const title = String(action?.title ?? (atype ? `ui_action: ${atype}` : "ui_action"));
  const toolCallId = String(data?.tool_call_id ?? "");

  if (atype === "request_client_state" || atype === "request_state") {
    const queryId = String(action?.query_id ?? toolCallId ?? "").trim();
    const canAck = typeof sessionId === "string" && sessionId.trim().length > 0 && !!queryId;
    const ackKey = toolCallId ? `client_state:${toolCallId}` : `client_state:${queryId}`;

    const gatherMediaSnapshot = (): any[] => {
      if (typeof document === "undefined") return [];
      const els = Array.from(document.querySelectorAll("audio,video")).slice(0, 20);
      return els.map((el) => {
        const isVideo = el.tagName.toLowerCase() === "video";
        const m: any = {
          kind: isVideo ? "video" : "audio",
          paused: (el as any).paused,
          ended: (el as any).ended,
        };
        const src = (el as HTMLMediaElement).currentSrc || (el as HTMLMediaElement).src || "";
        if (src) {
          m.src = safeTrunc(src, 300);
          const u = tryParseUrl(src);
          if (u && u.pathname.endsWith("/api/v1/file")) {
            const p = u.searchParams.get("path") || "";
            if (p) m.path = safeTrunc(p, 200);
          }
        }
        const ds: any = (el as any).dataset || {};
        if (typeof ds.toolCallId === "string" && ds.toolCallId.length > 0) m.tool_call_id = ds.toolCallId;
        if (typeof ds.path === "string" && ds.path.length > 0) m.path = safeTrunc(ds.path, 200);

        const ct = (el as any).currentTime;
        if (typeof ct === "number" && Number.isFinite(ct)) m.current_time = ct;
        const dur = (el as any).duration;
        if (typeof dur === "number" && Number.isFinite(dur)) m.duration = dur;
        return m;
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
    const msg = String(action?.message ?? "");
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
