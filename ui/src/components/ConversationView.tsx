import React from "react";
import { apiPostSessionUiEvent, extractSessionErrorMessage, type AgentEvent, type ApiAuth } from "../api";
import { parseAssistantMultimodal, renderAssistantMultimodal } from "./AssistantMultimodal";
import ConversationCard from "./ConversationCard";
import Markdown from "./Markdown";
import ToolResultView from "./ToolResultView";
import ArtifactView from "./ArtifactView";
import LlmDebugView from "./LlmDebugView";
import ConversationUiActionCard, { type ConversationRpcRuntime } from "./conversation/ConversationUiActionCard";
import {
  normalizeEventData,
  prettyJsonOrRaw,
  safeJsonParse,
  safeObject,
  safeTrunc,
} from "./conversation/utils";

const Card = ConversationCard;

const MEDIA_OBSERVER_TTL_MS = 15 * 60 * 1000;
const MEDIA_OBSERVER_MAX = 32;

export default function ConversationView({
  baseUrl,
  yolo,
  sessionId,
  client,
  daemonAuth,
  prompt,
  events,
  showDebugEvents,
  allowAutoplay,
  allowClientRpcs,
  allowClientEffects,
  allowUnsafePageEval,
  reverseOrder,
  disableAutoClientRpcs,
  sceneEntities,
  onSceneApply,
}: {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  client?: { id?: string; kind?: string; instance_id?: string };
  daemonAuth?: ApiAuth;
  prompt: string;
  events: AgentEvent[];
  showDebugEvents?: boolean;
  allowAutoplay: boolean;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  reverseOrder?: boolean;
  disableAutoClientRpcs?: boolean;
  sceneEntities?: any[];
  onSceneApply?: (ops: any[]) => any;
}) {
  const [ackError, setAckError] = React.useState<string | null>(null);
  const [ackedKeys, setAckedKeys] = React.useState<Record<string, number>>({});
  const ackedKeyLimit = 2000;
  const shownUiActionRef = React.useRef<Record<string, number>>({});
  const autoSceneApplyRef = React.useRef<Record<string, number>>({});
  const localUiActionLimit = 2000;

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

  const markAckedKey = React.useCallback(
    (key: string) => {
      if (!key) return;
      setAckedKeys((prev) => {
        const next: Record<string, number> = { ...prev, [key]: Date.now() };
        const keys = Object.keys(next);
        if (keys.length <= ackedKeyLimit) return next;
        keys.sort((a, b) => (next[a] || 0) - (next[b] || 0));
        const overflow = keys.length - ackedKeyLimit;
        for (let i = 0; i < overflow; i += 1) {
          delete next[keys[i]];
        }
        return next;
      });
    },
    [ackedKeyLimit],
  );

  const markSeenWithLimit = (store: Record<string, number>, key: string, limit: number) => {
    if (!key) return;
    store[key] = Date.now();
    const keys = Object.keys(store);
    if (keys.length <= limit) return;
    const items = keys
      .map((k) => ({ k, ts: store[k] || 0 }))
      .sort((a, b) => a.ts - b.ts);
    const overflow = items.length - limit;
    for (let i = 0; i < overflow; i += 1) {
      delete store[items[i].k];
    }
  };

  // Fundamental DoD handshake: when the UI renders a derived ui_action event, emit a client event so the agent
  // can deterministically stop repeating the same “show/play/notify” requests.
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
      markSeenWithLimit(shownUiActionRef.current, toolCallId, localUiActionLimit);
      void postClientEvent("ui_action_shown", { tool_call_id: toolCallId, action_type: atype, title: action?.title }).catch(() => {});
    });
  }, [events, postClientEvent, sessionId]);

  // Auto-run: entity_apply should update the Scene (collaboration surface). In production, do this in an effect
  // (not during render) so it reliably runs and so failures can be surfaced via client_rpc_result.
  React.useEffect(() => {
    if (disableAutoClientRpcs) return;
    if (!allowClientRpcs) return;
    if (!allowClientEffects) return;
    if (!onSceneApply) return;
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    const safeTruncRaw = (s: string, max: number) => (s.length > max ? s.slice(0, max) : s);

    const entityApplyArgsToOps = (args: any): any[] => {
      // Preferred shape: explicit ops array (create/update/delete/action/clear).
      if (Array.isArray(args?.ops)) return args.ops;

      // Compatibility: accept an "entities" list (alternate schema).
      if (Array.isArray(args?.entities)) {
        const ops: any[] = [];
        const ents = args.entities as any[];
        for (const ent of ents.slice(0, 50)) {
          if (!ent || typeof ent !== "object") continue;
          const id = safeTruncRaw(String(ent?.id ?? ""), 200);
          const entityKind = safeTruncRaw(String(ent?.entity_kind ?? ent?.entityKind ?? ent?.type ?? ent?.kind ?? ""), 100);
          if (!id || !entityKind) continue;
          const title = typeof ent?.title === "string" ? safeTruncRaw(String(ent.title), 200) : undefined;
          const props = safeObject(ent?.props ?? ent ?? {});
          ops.push({ op: "create", id, entity_kind: entityKind, title, props });
          const actions = Array.isArray(ent?.actions) ? ent.actions : [];
          for (const a of actions.slice(0, 20)) {
            const name = safeTruncRaw(String(a?.name ?? a?.action ?? a?.kind ?? ""), 80);
            if (!name) continue;
            ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? a ?? {}) });
          }
        }
        return ops;
      }

      // Compatibility: accept the older "id/type/props/actions" shorthand.
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
      for (const a of actions.slice(0, 20)) {
        const name = safeTruncRaw(String(a?.name ?? a?.action ?? ""), 80);
        if (!name) continue;
        ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? {}) });
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

    const capForEvent = (v: any) => {
      try {
        const s = JSON.stringify(v);
        const max = 32 * 1024;
        if (s.length <= max) return v;
        return { kind: "truncated", bytes: s.length, preview: s.slice(0, 2000) };
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
      markSeenWithLimit(autoSceneApplyRef.current, ackKey, localUiActionLimit);

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
      } catch (e) {
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: false,
          elapsed_ms: 0,
          error: String(e),
        }).catch(() => {});
        if (atype === "client_probe") {
          void postClientEvent("client_probe_result", {
            probe_id: rpcId,
            request_tool_call_id: toolCallId,
            probe_kind: rpcKind,
            ok: false,
            elapsed_ms: 0,
            error: String(e),
          }).catch(() => {});
        }
      }
    });
  }, [allowClientEffects, allowClientRpcs, disableAutoClientRpcs, events, onSceneApply, postClientEvent, sessionId]);

  const toolCallSummaryById = React.useMemo(() => {
    const m: Record<string, { cmd?: string; argv?: string }> = {};
    for (const ev of events) {
      if (ev.type !== "tool_result") continue;
      const data: any = normalizeEventData(ev.data);
      const toolCallId = typeof data?.tool_call_id === "string" ? String(data.tool_call_id) : "";
      if (!toolCallId) continue;
      const summary = data?.summary && typeof data.summary === "object" ? data.summary : null;
      if (!summary) continue;
      const cmd = typeof summary?.cmd === "string" ? String(summary.cmd) : "";
      const argvArr = Array.isArray(summary?.argv) ? summary.argv : null;
      const argv =
        argvArr && argvArr.length > 0
          ? argvArr.map((x: any) => (typeof x === "string" ? x : "")).filter((x: string) => x.length > 0).join(" ")
          : "";
      if (cmd || argv) {
        m[toolCallId] = { cmd: cmd || undefined, argv: argv || undefined };
      }
    }
    return m;
  }, [events]);

  const items: Array<React.ReactNode> = [];
  let streamedAssistant = "";
  let sawFinalAssistant = false;
  let sawToolOrAssistant = false;
  let lastHeartbeat: any = null;

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
    localUiActionLimit,
  };

  React.useEffect(() => {
    const pending = pendingAutoRunsRef.current || {};
    const keys = Object.keys(pending);
    if (keys.length === 0) return;
    pendingAutoRunsRef.current = {};
    keys.forEach((k) => {
      try {
        pending[k]?.();
      } catch {
        // ignore
      }
    });
  });

  React.useEffect(() => {
    return () => {
      const all = rpcCleanupRef.current || {};
      Object.keys(all).forEach((k) => {
        cleanupRpcEntry(k);
      });
      rpcCleanupRef.current = {};

      // Best-effort cleanup of blob: URLs created by artifact_url RPCs.
      for (const u of artifactBlobUrlsRef.current) {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      }
      artifactBlobUrlsRef.current = [];
    };
  }, []);

  React.useEffect(() => {
    if (typeof window === "undefined") return;
    const t = window.setInterval(() => {
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
          const victim = activeMedia[i];
          cleanupRpcEntry(victim.id);
        }
      }
    }, 30_000);
    return () => {
      try {
        window.clearInterval(t);
      } catch {
        // ignore
      }
    };
  }, [cleanupRpcEntry]);

  if (prompt.trim().length > 0) {
    items.push(
      <Card key="user" title="User">
        <Markdown text={prompt} />
      </Card>,
    );
  }

  events.forEach((ev, idx) => {
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
          const argv = Array.isArray(argvRaw) ? argvRaw.map((x) => (typeof x === "string" ? x : "")).filter((x) => x.length > 0) : [];
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
      const title = `Tool call: ${name || "(unknown)"}`;
      items.push(
        <Card key={`tc-${idx}`} title={title}>
          {inferredSummary ? (
            <div className="mb-2">
              <div className="mb-1 text-[11px] font-semibold text-white/60">Command</div>
              <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                {inferredSummary.value}
              </pre>
            </div>
          ) : null}
          <details>
            <summary className="cursor-pointer text-[11px] text-white/60">
              Arguments (collapsed)
            </summary>
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
      const commandLine =
        summaryFromResult?.cmd ? summaryFromResult.cmd : summaryFromResult?.argv ? summaryFromResult.argv : "";
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

    // Hide low-level transport/debug events by default; those remain visible in the “Events” timeline.
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

  // If the daemon is emitting only low-level events (e.g. llm_request/llm_response/start) the conversation view
  // can look "stuck" even though the job is progressing. Provide a lightweight status card in that case.
  if (!sawToolOrAssistant && events.length > 0) {
    const hb = lastHeartbeat && typeof lastHeartbeat === "object" ? JSON.stringify(lastHeartbeat, null, 2) : "";
    items.push(
      <Card key="working" title="Working…">
        <div className="text-xs text-white/70">
          Waiting for assistant output / tool calls.
          {hb ? (
            <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-[11px] leading-relaxed text-white/80">
              {hb}
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
  return <div className="grid gap-3">{displayItems}</div>;
}
