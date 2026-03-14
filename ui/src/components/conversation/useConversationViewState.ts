import React from "react";

import { apiPostSessionUiEvent, extractSessionErrorMessage } from "../../api";
import { normalizeEventData, safeObject } from "./utils";
import type { ConversationRpcRuntime, ConversationToolCallSummaryById, ConversationViewProps } from "./conversationViewTypes";

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
  const rpcCleanupRef = React.useRef<
    Record<
      string,
      {
        cleanups: Array<() => void>;
        kind: string;
        createdMs: number;
        lastActiveMs: number;
      }
    >
  >({});
  const artifactBlobUrlsRef = React.useRef<string[]>([]);

  const postClientEvent = React.useCallback(
    async (type: string, data: any) => {
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
      const data: any = normalizeEventData(ev.data);
      const toolCallId = String(data?.tool_call_id ?? "");
      if (!toolCallId) return;
      if (shownUiActionRef.current[toolCallId]) return;
      const action = data?.action ?? {};
      const atype = String(action?.type ?? "");
      markSeenWithLimit(shownUiActionRef.current, toolCallId, LOCAL_UI_ACTION_LIMIT);
      void postClientEvent("ui_action_shown", { tool_call_id: toolCallId, action_type: atype, title: action?.title }).catch(() => {});
    });
  }, [events, markSeenWithLimit, postClientEvent, sessionId]);

  React.useEffect(() => {
    if (disableAutoClientRpcs) return;
    if (!allowClientRpcs) return;
    if (!allowClientEffects) return;
    if (!onSceneApply) return;
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;

    const safeTruncRaw = (value: string, max: number) => (value.length > max ? value.slice(0, max) : value);

    const entityApplyArgsToOps = (args: any): any[] => {
      if (Array.isArray(args?.ops)) return args.ops;

      if (Array.isArray(args?.entities)) {
        const ops: any[] = [];
        for (const entity of (args.entities as any[]).slice(0, 50)) {
          if (!entity || typeof entity !== "object") continue;
          const id = safeTruncRaw(String(entity?.id ?? ""), 200);
          const entityKind = safeTruncRaw(String(entity?.entity_kind ?? entity?.entityKind ?? entity?.type ?? entity?.kind ?? ""), 100);
          if (!id || !entityKind) continue;
          const title = typeof entity?.title === "string" ? safeTruncRaw(String(entity.title), 200) : undefined;
          const props = safeObject(entity?.props ?? entity ?? {});
          ops.push({ op: "create", id, entity_kind: entityKind, title, props });
          const actions = Array.isArray(entity?.actions) ? entity.actions : [];
          for (const action of actions.slice(0, 20)) {
            const name = safeTruncRaw(String(action?.name ?? action?.action ?? action?.kind ?? ""), 80);
            if (!name) continue;
            ops.push({ op: "action", id, action: name, args: safeObject(action?.args ?? action ?? {}) });
          }
        }
        return ops;
      }

      const id = safeTruncRaw(String(args?.id ?? ""), 200);
      const entityKind = safeTruncRaw(String(args?.entity_kind ?? args?.entityKind ?? args?.type ?? args?.kind ?? ""), 100);
      const props = safeObject(args?.props ?? {});
      const titleFromProps = typeof (props as any)?.title === "string" ? safeTruncRaw(String((props as any).title), 200) : "";
      const titleFromArgs = typeof args?.title === "string" ? safeTruncRaw(String(args.title), 200) : "";
      const title = titleFromArgs || titleFromProps || "";

      const ops: any[] = [];
      if (id && entityKind && (Object.keys(props).length > 0 || title)) {
        ops.push({ op: "create", id, entity_kind: entityKind, title: title || undefined, props });
      } else if (id && Object.keys(props).length > 0) {
        ops.push({ op: "update", id, props });
      }
      const actions = Array.isArray(args?.actions) ? args.actions : [];
      for (const action of actions.slice(0, 20)) {
        const name = safeTruncRaw(String(action?.name ?? action?.action ?? ""), 80);
        if (!name) continue;
        ops.push({ op: "action", id, action: name, args: safeObject(action?.args ?? {}) });
      }
      const singleAction = safeTruncRaw(String(args?.action ?? ""), 80);
      if (singleAction) {
        ops.push({ op: "action", id, action: singleAction, args: safeObject(args?.args ?? {}) });
      }
      if (args?.delete === true || args?.remove === true) {
        ops.push({ op: "delete", id });
      }
      if (args?.clear === true) {
        throw new Error("scene clear is disabled in WebUI");
      }
      return ops;
    };

    const capForEvent = (value: any) => {
      try {
        const serialized = JSON.stringify(value);
        const max = 32 * 1024;
        if (serialized.length <= max) return value;
        return { kind: "truncated", bytes: serialized.length, preview: serialized.slice(0, 2000) };
      } catch {
        return { kind: "unserializable" };
      }
    };

    events.forEach((ev, idx) => {
      if (ev.type !== "ui_action") return;
      const data: any = normalizeEventData(ev.data);
      const toolCallId = String(data?.tool_call_id ?? "").trim();
      const action = data?.action ?? {};
      const atype = String(action?.type ?? "").trim();
      if (atype !== "client_rpc" && atype !== "collab_rpc" && atype !== "client_probe") return;

      const rpcId = String(action?.rpc_id ?? action?.probe_id ?? toolCallId ?? "").trim();
      const rpc = action?.rpc ?? action?.probe ?? {};
      const rpcKind = String(rpc?.kind ?? "").trim();
      if (rpcKind !== "entity_apply") return;

      const autoRunRequested =
        typeof action?.auto_run === "boolean" ? action.auto_run : typeof action?.auto === "boolean" ? action.auto : true;
      if (!autoRunRequested) return;
      if (!rpcId) return;

      const ackKey = `entity_apply:${toolCallId || rpcId || idx}`;
      if (autoSceneApplyRef.current[ackKey]) return;
      markSeenWithLimit(autoSceneApplyRef.current, ackKey, LOCAL_UI_ACTION_LIMIT);

      const rpcArgs = typeof rpc?.args === "object" && rpc?.args ? rpc.args : rpc;
      const ops = entityApplyArgsToOps(rpcArgs);
      try {
        const result = onSceneApply(ops);
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: true,
          elapsed_ms: 0,
          result: capForEvent(result),
        }).catch(() => {});
        if (atype === "client_probe") {
          void postClientEvent("client_probe_result", {
            probe_id: rpcId,
            request_tool_call_id: toolCallId,
            probe_kind: rpcKind,
            ok: true,
            elapsed_ms: 0,
            result: capForEvent(result),
          }).catch(() => {});
        }
      } catch (error) {
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: false,
          elapsed_ms: 0,
          error: String(error),
        }).catch(() => {});
        if (atype === "client_probe") {
          void postClientEvent("client_probe_result", {
            probe_id: rpcId,
            request_tool_call_id: toolCallId,
            probe_kind: rpcKind,
            ok: false,
            elapsed_ms: 0,
            error: String(error),
          }).catch(() => {});
        }
      }
    });
  }, [allowClientEffects, allowClientRpcs, disableAutoClientRpcs, events, markSeenWithLimit, onSceneApply, postClientEvent, sessionId]);

  const toolCallSummaryById = React.useMemo<ConversationToolCallSummaryById>(() => {
    const summaries: ConversationToolCallSummaryById = {};
    for (const ev of events) {
      if (ev.type !== "tool_result") continue;
      const data: any = normalizeEventData(ev.data);
      const toolCallId = typeof data?.tool_call_id === "string" ? String(data.tool_call_id) : "";
      if (!toolCallId) continue;
      const summary = data?.summary && typeof data.summary === "object" ? data.summary : null;
      if (!summary) continue;
      const cmd = typeof summary?.cmd === "string" ? String(summary.cmd) : "";
      const argvRaw = Array.isArray(summary?.argv) ? summary.argv : null;
      const argv =
        argvRaw && argvRaw.length > 0
          ? argvRaw.map((value: any) => (typeof value === "string" ? value : "")).filter(Boolean).join(" ")
          : "";
      if (cmd || argv) {
        summaries[toolCallId] = { cmd: cmd || undefined, argv: argv || undefined };
      }
    }
    return summaries;
  }, [events]);

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
