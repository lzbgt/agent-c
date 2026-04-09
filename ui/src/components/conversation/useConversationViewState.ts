import React from "react";

import { apiPostSessionUiEvent, extractSessionErrorMessage } from "../../api";
import {
  buildConversationToolCallSummaryById,
  buildEntityApplyOps,
  capEventPayload,
  getNormalizedEventRecord,
  parseConversationUiAction,
} from "./conversationData";
import type { ConversationRpcRuntime, ConversationViewProps, RpcCleanupEntry } from "./conversationViewTypes";

const ACKED_KEY_LIMIT = 2000;
const LOCAL_UI_ACTION_LIMIT = 2000;
const MEDIA_OBSERVER_TTL_MS = 15 * 60 * 1000;
const MEDIA_OBSERVER_MAX = 32;

export default function useConversationViewState({
  baseUrl,
  client,
  daemonAuth,
  events,
  sessionId,
  allowClientEffects,
  allowClientRpcs,
  disableAutoClientRpcs,
  onSceneApply,
}: Pick<
  ConversationViewProps,
  | "baseUrl"
  | "client"
  | "daemonAuth"
  | "events"
  | "sessionId"
  | "allowClientEffects"
  | "allowClientRpcs"
  | "disableAutoClientRpcs"
  | "onSceneApply"
>) {
  const [ackError, setAckError] = React.useState<string | null>(null);
  const [ackedKeys, setAckedKeys] = React.useState<Record<string, number>>({});
  const shownUiActionRef = React.useRef<Record<string, number>>({});
  const autoSceneApplyRef = React.useRef<Record<string, number>>({});
  const probeRanRef = React.useRef<Record<string, number>>({});
  const pendingAutoRunsRef = React.useRef<Record<string, () => void>>({});
  const rpcCleanupRef = React.useRef<Record<string, RpcCleanupEntry>>({});
  const artifactBlobUrlsRef = React.useRef<string[]>([]);

  const postClientEvent = React.useCallback(
    async (type: string, data: unknown) => {
      const sid = typeof sessionId === "string" ? sessionId.trim() : "";
      if (sid.length === 0) {
        throw new Error("missing session_id");
      }
      const resp = await apiPostSessionUiEvent(
        baseUrl,
        {
          session_id: sid,
          type,
          client: client ?? { id: "webui", kind: "webui" },
          data: data ?? {},
          append_to_session: false,
        },
        daemonAuth,
      );
      if (!resp.ok) {
        throw new Error(extractSessionErrorMessage(resp) || "client_event failed");
      }
    },
    [baseUrl, client, daemonAuth, sessionId],
  );

  const markAckedKey = React.useCallback((key: string) => {
    if (!key) return;
    setAckedKeys((prev) => {
      const next: Record<string, number> = { ...prev, [key]: Date.now() };
      const keys = Object.keys(next);
      if (keys.length <= ACKED_KEY_LIMIT) return next;
      keys.sort((a, b) => (next[a] || 0) - (next[b] || 0));
      const overflow = keys.length - ACKED_KEY_LIMIT;
      for (let i = 0; i < overflow; i += 1) {
        delete next[keys[i]];
      }
      return next;
    });
  }, []);

  const markSeenWithLimit = React.useCallback((store: Record<string, number>, key: string, limit: number) => {
    if (!key) return;
    store[key] = Date.now();
    const keys = Object.keys(store);
    if (keys.length <= limit) return;
    const items = keys
      .map((item) => ({ item, ts: store[item] || 0 }))
      .sort((a, b) => a.ts - b.ts);
    const overflow = items.length - limit;
    for (let i = 0; i < overflow; i += 1) {
      delete store[items[i].item];
    }
  }, []);

  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (sid.length === 0) return;
    events.forEach((ev) => {
      if (ev.type !== "ui_action") return;
      const parsed = parseConversationUiAction(getNormalizedEventRecord(ev.data));
      const toolCallId = parsed.toolCallId;
      if (!toolCallId) return;
      if (shownUiActionRef.current[toolCallId]) return;
      markSeenWithLimit(shownUiActionRef.current, toolCallId, LOCAL_UI_ACTION_LIMIT);
      void postClientEvent("ui_action_shown", {
        tool_call_id: toolCallId,
        action_type: parsed.atype,
        title: parsed.title,
      }).catch(() => {});
    });
  }, [events, markSeenWithLimit, postClientEvent, sessionId]);

  React.useEffect(() => {
    if (disableAutoClientRpcs) return;
    if (!allowClientRpcs) return;
    if (!allowClientEffects) return;
    if (!onSceneApply) return;
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;

    events.forEach((ev, idx) => {
      if (ev.type !== "ui_action") return;
      const parsed = parseConversationUiAction(getNormalizedEventRecord(ev.data));
      if (!(parsed.atype === "client_rpc" || parsed.atype === "collab_rpc" || parsed.atype === "client_probe")) return;
      if (parsed.rpcKind !== "entity_apply") return;
      if (!parsed.autoRunRequested) return;
      const toolCallId = parsed.toolCallId;
      const rpcId = parsed.rpcId;
      if (!rpcId) return;

      const ackKey = `entity_apply:${toolCallId || rpcId || idx}`;
      if (autoSceneApplyRef.current[ackKey]) return;
      markSeenWithLimit(autoSceneApplyRef.current, ackKey, LOCAL_UI_ACTION_LIMIT);

      try {
        const ops = buildEntityApplyOps(parsed.rpcArgs);
        if (ops.some((op) => op.op === "clear")) {
          throw new Error("scene clear is disabled in WebUI");
        }
        const result = onSceneApply(ops);
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: parsed.rpcKind,
          ok: true,
          elapsed_ms: 0,
          result: capEventPayload(result),
        }).catch(() => {});
        if (parsed.atype === "client_probe") {
          void postClientEvent("client_probe_result", {
            probe_id: rpcId,
            request_tool_call_id: toolCallId,
            probe_kind: parsed.rpcKind,
            ok: true,
            elapsed_ms: 0,
            result: capEventPayload(result),
          }).catch(() => {});
        }
      } catch (error) {
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: parsed.rpcKind,
          ok: false,
          elapsed_ms: 0,
          error: String(error),
        }).catch(() => {});
        if (parsed.atype === "client_probe") {
          void postClientEvent("client_probe_result", {
            probe_id: rpcId,
            request_tool_call_id: toolCallId,
            probe_kind: parsed.rpcKind,
            ok: false,
            elapsed_ms: 0,
            error: String(error),
          }).catch(() => {});
        }
      }
    });
  }, [allowClientEffects, allowClientRpcs, disableAutoClientRpcs, events, markSeenWithLimit, onSceneApply, postClientEvent, sessionId]);

  const toolCallSummaryById = React.useMemo(() => buildConversationToolCallSummaryById(events), [events]);

  const cleanupRpcEntry = React.useCallback((id: string) => {
    const entry = rpcCleanupRef.current[id];
    if (!entry) return false;
    entry.cleanups.forEach((fn) => {
      try {
        fn();
      } catch {
        // ignore
      }
    });
    delete rpcCleanupRef.current[id];
    return true;
  }, []);

  const rpcRuntime: ConversationRpcRuntime = {
    pendingAutoRunsRef,
    probeRanRef,
    rpcCleanupRef,
    artifactBlobUrlsRef,
    cleanupRpcEntry,
    markSeenWithLimit,
    localUiActionLimit: LOCAL_UI_ACTION_LIMIT,
  };

  React.useEffect(() => {
    const pending = pendingAutoRunsRef.current || {};
    const keys = Object.keys(pending);
    if (keys.length === 0) return;
    pendingAutoRunsRef.current = {};
    keys.forEach((key) => {
      try {
        pending[key]?.();
      } catch {
        // ignore
      }
    });
  });

  React.useEffect(() => {
    return () => {
      const all = rpcCleanupRef.current || {};
      Object.keys(all).forEach((key) => {
        cleanupRpcEntry(key);
      });
      rpcCleanupRef.current = {};

      for (const url of artifactBlobUrlsRef.current) {
        try {
          URL.revokeObjectURL(url);
        } catch {
          // ignore
        }
      }
      artifactBlobUrlsRef.current = [];
    };
  }, [cleanupRpcEntry]);

  React.useEffect(() => {
    if (typeof window === "undefined") return;
    const timer = window.setInterval(() => {
      const entries = rpcCleanupRef.current;
      const ids = Object.keys(entries);
      if (ids.length === 0) return;
      const now = Date.now();

      const activeMedia: Array<{ id: string; lastActiveMs: number }> = [];
      ids.forEach((id) => {
        const entry = entries[id];
        if (!entry) return;
        if (entry.kind === "media_observe" && now - entry.lastActiveMs > MEDIA_OBSERVER_TTL_MS) {
          cleanupRpcEntry(id);
          return;
        }
        if (entry.kind === "media_observe") {
          activeMedia.push({ id, lastActiveMs: entry.lastActiveMs });
        }
      });

      if (MEDIA_OBSERVER_MAX > 0 && activeMedia.length > MEDIA_OBSERVER_MAX) {
        activeMedia.sort((a, b) => a.lastActiveMs - b.lastActiveMs);
        const overflow = activeMedia.length - MEDIA_OBSERVER_MAX;
        for (let i = 0; i < overflow; i += 1) {
          cleanupRpcEntry(activeMedia[i].id);
        }
      }
    }, 30_000);
    return () => {
      try {
        window.clearInterval(timer);
      } catch {
        // ignore
      }
    };
  }, [cleanupRpcEntry]);

  return {
    ackError,
    ackedKeys,
    markAckedKey,
    postClientEvent,
    rpcRuntime,
    setAckError,
    toolCallSummaryById,
  };
}
