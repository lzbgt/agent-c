import React from "react";
import { apiPostSessionUiEvent, type AgentEvent } from "../api";
import Markdown from "./Markdown";
import ToolResultView from "./ToolResultView";
import ArtifactView from "./ArtifactView";
import LlmDebugView from "./LlmDebugView";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function normalizeEventData(data: unknown): any {
  if (typeof data === "string") {
    return safeJsonParse(data) ?? data;
  }
  return data ?? {};
}

function Card({
  title,
  children,
}: {
  title: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-1.5">
        <div className="text-sm font-semibold">{title}</div>
      </div>
      <div className="px-3 pb-2">{children}</div>
    </div>
  );
}

function prettyJsonOrRaw(s: string) {
  const parsed = safeJsonParse(s);
  if (!parsed) return s;
  return JSON.stringify(parsed, null, 2);
}

function safeTrunc(s: string, max: number): string {
  if (s.length <= max) return s;
  return s.slice(0, Math.max(0, max - 1)) + "…";
}

function clampInt(n: unknown, lo: number, hi: number, def: number): number {
  const v = typeof n === "number" ? n : Number(n);
  if (!Number.isFinite(v)) return def;
  return Math.min(Math.max(Math.trunc(v), lo), hi);
}

function safeObject(v: any): Record<string, any> {
  if (!v || typeof v !== "object" || Array.isArray(v)) return {};
  return v as Record<string, any>;
}

function isSensitiveKey(k: string): boolean {
  const s = String(k || "").toLowerCase();
  return (
    s.includes("secret") ||
    s.includes("token") ||
    s.includes("auth") ||
    s.includes("apikey") ||
    s.includes("api_key") ||
    s.includes("password") ||
    s.includes("passwd") ||
    s.includes("session") ||
    s.includes("cookie")
  );
}

function tryParseUrl(s: string): URL | null {
  try {
    return new URL(s);
  } catch {
    return null;
  }
}

function globalAutoRunOnceMap(): Record<string, boolean> {
  // React StrictMode in dev may mount/unmount/mount components, which resets refs.
  // Use a tiny global cache to avoid auto-running the same client RPC twice per page load.
  const g = globalThis as any;
  if (!g.__agentui_auto_run_once || typeof g.__agentui_auto_run_once !== "object") {
    g.__agentui_auto_run_once = {};
  }
  const m = g.__agentui_auto_run_once as Record<string, boolean>;
  try {
    const n = Object.keys(m).length;
    if (n > 2000) g.__agentui_auto_run_once = {};
  } catch {
    // ignore
  }
  return (g.__agentui_auto_run_once as Record<string, boolean>) || {};
}

function createInlineWorker(source: string): Worker {
  const blob = new Blob([source], { type: "text/javascript" });
  const url = URL.createObjectURL(blob);
  const w = new Worker(url);
  // Best-effort: release URL immediately; Worker holds its own reference.
  try {
    URL.revokeObjectURL(url);
  } catch {
    // ignore
  }
  return w;
}

export default function ConversationView({
  baseUrl,
  yolo,
  sessionId,
  client,
  daemonAuthToken,
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
  daemonAuthToken?: string;
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
  const [ackedKeys, setAckedKeys] = React.useState<Record<string, boolean>>({});
  const shownUiActionRef = React.useRef<Record<string, boolean>>({});
  const autoSceneApplyRef = React.useRef<Record<string, boolean>>({});

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
        daemonAuthToken,
      );
      if (!resp.ok) {
        throw new Error(resp.error || "client_event failed");
      }
    },
    [baseUrl, client, daemonAuthToken, sessionId],
  );

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
      shownUiActionRef.current[toolCallId] = true;
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

    const safeTrunc = (s: string, max: number) => (s.length > max ? s.slice(0, max) : s);
    const safeObject = (v: any) => (v && typeof v === "object" && !Array.isArray(v) ? v : {});

    const entityApplyArgsToOps = (args: any): any[] => {
      // Preferred shape: explicit ops array (create/update/delete/action/clear).
      if (Array.isArray(args?.ops)) return args.ops;

      // Compatibility: accept an "entities" list (alternate schema).
      if (Array.isArray(args?.entities)) {
        const ops: any[] = [];
        const ents = args.entities as any[];
        for (const ent of ents.slice(0, 50)) {
          if (!ent || typeof ent !== "object") continue;
          const id = safeTrunc(String(ent?.id ?? ""), 200);
          const entityKind = safeTrunc(String(ent?.entity_kind ?? ent?.entityKind ?? ent?.type ?? ent?.kind ?? ""), 100);
          if (!id || !entityKind) continue;
          const title = typeof ent?.title === "string" ? safeTrunc(String(ent.title), 200) : undefined;
          const props = safeObject(ent?.props ?? ent ?? {});
          ops.push({ op: "create", id, entity_kind: entityKind, title, props });
          const actions = Array.isArray(ent?.actions) ? ent.actions : [];
          for (const a of actions.slice(0, 20)) {
            const name = safeTrunc(String(a?.name ?? a?.action ?? a?.kind ?? ""), 80);
            if (!name) continue;
            ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? a ?? {}) });
          }
        }
        return ops;
      }

      // Compatibility: accept the older "id/type/props/actions" shorthand.
      const id = safeTrunc(String(args?.id ?? ""), 200);
      const entityKind = safeTrunc(String(args?.entity_kind ?? args?.entityKind ?? args?.type ?? args?.kind ?? ""), 100);
      const props = safeObject(args?.props ?? {});
      const titleFromProps = typeof (props as any)?.title === "string" ? safeTrunc(String((props as any).title), 200) : "";
      const titleFromArgs = typeof args?.title === "string" ? safeTrunc(String(args.title), 200) : "";
      const title = titleFromArgs || titleFromProps || "";

      const ops: any[] = [];
      if (id && entityKind && (Object.keys(props).length > 0 || title)) {
        ops.push({ op: "create", id, entity_kind: entityKind, title: title || undefined, props });
      } else if (id && Object.keys(props).length > 0) {
        ops.push({ op: "update", id, props });
      }
      const actions = Array.isArray(args?.actions) ? args.actions : [];
      for (const a of actions.slice(0, 20)) {
        const name = safeTrunc(String(a?.name ?? a?.action ?? ""), 80);
        if (!name) continue;
        ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? {}) });
      }
      const singleAction = safeTrunc(String(args?.action ?? ""), 80);
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
      autoSceneApplyRef.current[ackKey] = true;

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

  const probeRanRef = React.useRef<Record<string, boolean>>({});
  const pendingAutoRunsRef = React.useRef<Record<string, () => void>>({});
  const rpcCleanupRef = React.useRef<Record<string, Array<() => void>>>({});
  const artifactBlobUrlsRef = React.useRef<string[]>([]);

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
        const cleanups = all[k] || [];
        cleanups.forEach((fn) => {
          try {
            fn();
          } catch {
            // ignore
          }
        });
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
      items.push(
        <Card key={`a-${idx}`} title="Assistant">
          <Markdown text={String(data.assistant_content ?? "")} />
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
      const title = (() => {
        if (!inferredSummary) return `Tool call: ${name || "(unknown)"}`;
        return `Tool call: ${name || "(unknown)"} — ${inferredSummary.label}: ${safeTrunc(String(inferredSummary.value || ""), 120)}`;
      })();
      items.push(
        <Card key={`tc-${idx}`} title={title}>
          {inferredSummary ? (
          <pre className="mb-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
            {inferredSummary.value}
          </pre>
          ) : null}
          <pre className="overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {args ? prettyJsonOrRaw(args) : inferredSummary ? "(arguments omitted; see command above)" : "(enable verbose to capture arguments)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "tool_result") {
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      const toolCallId = typeof data?.tool_call_id === "string" ? String(data.tool_call_id) : "";
      if (typeof data.content === "string") {
        items.push(
          <div key={`tr-${idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
            <ToolResultView
              baseUrl={baseUrl}
              yolo={yolo}
              daemonAuthToken={daemonAuthToken}
              sessionId={sessionId}
              toolCallId={toolCallId}
              content={data.content}
            />
          </div>,
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
          <div key={`tr-${idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
            <ToolResultView
              baseUrl={baseUrl}
              yolo={yolo}
              daemonAuthToken={daemonAuthToken}
              sessionId={sessionId}
              toolCallId={toolCallId}
              content={envelope}
            />
          </div>,
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
            daemonAuthToken={daemonAuthToken}
          />
        </Card>,
      );
      return;
    }

    if (type === "ui_action") {
      sawToolOrAssistant = true;
      const action = data?.action ?? {};
      const atype = String(action?.type ?? "");
      const title = String(action?.title ?? (atype ? `ui_action: ${atype}` : "ui_action"));
      const toolCallId = String(data?.tool_call_id ?? "");

      if (atype === "client_rpc" || atype === "collab_rpc" || atype === "client_probe") {
        const rpcId = String(action?.rpc_id ?? action?.probe_id ?? toolCallId ?? "").trim();
        const rpc = action?.rpc ?? action?.probe ?? {};
        const rpcKind = String(rpc?.kind ?? "").trim();
        const rpcArgs = typeof rpc?.args === "object" && rpc?.args ? rpc.args : rpc;
        const sideEffectKinds = new Set([
          "dom_click",
          "dom_set_value",
          "dom_apply",
          "entity_apply",
          "media_play",
          "media_observe",
          "navigate",
          "page_eval",
        ]);
        const sideEffectsRequested = rpc?.side_effects === true || action?.side_effects === true || sideEffectKinds.has(rpcKind);

        const canRun = !!rpcId && typeof sessionId === "string" && sessionId.trim().length > 0;
        const canRunAuto = !!allowClientRpcs && (!sideEffectsRequested || !!allowClientEffects);
        const autoRunRequested =
          typeof action?.auto_run === "boolean" ? action.auto_run : typeof action?.auto === "boolean" ? action.auto : true;
        const autoRun = canRunAuto && autoRunRequested;

        const postRpcResult = async (ok: boolean, payload: any) => {
          const capForEvent = (v: any) => {
            // Prevent client event logs from ballooning (e.g. script_eval returning huge arrays).
            // Keep it coarse and predictable: if the JSON form is too large, replace with a bounded summary.
            try {
              const s = JSON.stringify(v);
              const max = 32 * 1024;
              if (s.length <= max) return v;
              return { kind: "truncated", bytes: s.length, preview: s.slice(0, 2000) };
            } catch {
              return { kind: "unserializable" };
            }
          };
          const base = {
            rpc_id: rpcId,
            request_tool_call_id: toolCallId,
            rpc_kind: rpcKind,
            ok,
            elapsed_ms: payload?.elapsed_ms,
          };
          const data = ok ? { ...base, result: capForEvent(payload?.result) } : { ...base, error: String(payload?.error ?? "") };
          await postClientEvent("client_rpc_result", data);
          // Legacy alias: if the request was a client_probe, emit the old event name too.
          if (atype === "client_probe") {
            const legacyBase = {
              probe_id: rpcId,
              request_tool_call_id: toolCallId,
              probe_kind: rpcKind,
              ok,
              elapsed_ms: payload?.elapsed_ms,
            };
            const legacyData = ok ? { ...legacyBase, result: capForEvent(payload?.result) } : { ...legacyBase, error: String(payload?.error ?? "") };
            await postClientEvent("client_probe_result", legacyData);
          }
        };

        const postRpcProgress = async (name: string, payload?: any) => {
          await postClientEvent("client_rpc_progress", {
            rpc_id: rpcId,
            rpc_kind: rpcKind,
            name: String(name || "progress"),
            ts_unix_ms: Date.now(),
            payload: payload ?? {},
          });
        };

        const runRpc = async () => {
          const t0 = Date.now();
          try {
            if (!canRun) throw new Error("missing session/rpc_id");
            if (!rpcKind) throw new Error("missing rpc.kind");
            if (!allowClientRpcs) throw new Error("client RPC disabled by settings");
            if (sideEffectsRequested && !allowClientEffects) throw new Error("client RPC side effects disabled by settings");
            const safeFieldSet = new Set([
              "tag",
              "text",
              "value",
              "checked",
              "dataset",
              "attrs",
              "currentSrc",
              "paused",
              "ended",
              "currentTime",
              "duration",
            ]);
            const makeDomQuery = (args: any) => {
              const selector = safeTrunc(String(args?.selector ?? ""), 200);
              const limit = clampInt(args?.limit, 1, 20, 10);
              const fieldsRaw = Array.isArray(args?.fields) ? args.fields : [];
              let fields = fieldsRaw
                .map((x: any) => String(x))
                .filter((f: string) => safeFieldSet.has(f))
                .slice(0, 20);
              if (fields.length === 0) fields = ["tag", "text"];
              if (!selector) throw new Error("dom_query requires selector");
              const els = Array.from(document.querySelectorAll(selector)).slice(0, limit);
              const items = els.map((el) => {
                const out: any = {};
                const asAny: any = el as any;
                if (fields.includes("tag")) out.tag = el.tagName.toLowerCase();
                if (fields.includes("text")) out.text = safeTrunc(String(el.textContent ?? "").trim(), 400);
                if (fields.includes("value") && "value" in asAny) {
                  const inputType =
                    typeof (asAny as HTMLInputElement)?.type === "string" ? String((asAny as HTMLInputElement).type).toLowerCase() : "";
                  if (inputType === "password") out.value = "(redacted)";
                  else out.value = safeTrunc(String(asAny.value ?? ""), 200);
                }
                if (fields.includes("checked") && "checked" in asAny) out.checked = !!asAny.checked;
                if (fields.includes("dataset")) {
                  const ds: any = asAny.dataset ?? {};
                  const keys = Object.keys(ds).slice(0, 20);
                  const dso: any = {};
                  keys.forEach((k: string) => {
                    dso[k] = isSensitiveKey(k) ? "(redacted)" : safeTrunc(String(ds[k] ?? ""), 200);
                  });
                  out.dataset = dso;
                }
                if (fields.includes("attrs")) {
                  const names = Array.from(el.getAttributeNames ? el.getAttributeNames() : []).slice(0, 20);
                  const attrs: any = {};
                  names.forEach((n) => {
                    attrs[n] = isSensitiveKey(n) ? "(redacted)" : safeTrunc(String(el.getAttribute(n) ?? ""), 200);
                  });
                  out.attrs = attrs;
                }
                if (fields.includes("currentSrc") && "currentSrc" in asAny) out.currentSrc = safeTrunc(String(asAny.currentSrc ?? ""), 300);
                if (fields.includes("paused") && "paused" in asAny) out.paused = !!asAny.paused;
                if (fields.includes("ended") && "ended" in asAny) out.ended = !!asAny.ended;
                if (fields.includes("currentTime") && "currentTime" in asAny) out.currentTime = asAny.currentTime;
                if (fields.includes("duration") && "duration" in asAny) out.duration = asAny.duration;
                return out;
              });
              return { kind: "dom_query", selector, limit, fields, items };
            };

            const makeDomApply = (args: any) => {
              const opsRaw = Array.isArray(args?.ops) ? args.ops : [];
              const ops = opsRaw.slice(0, 100);
              const results: any[] = [];

              const applyOne = (op: any) => {
                const o = safeObject(op);
                const kind = String(o.op ?? o.kind ?? "").trim();
                if (!kind) throw new Error("dom_apply op missing op");

                const limit = clampInt(o.limit, 1, 50, 1);

                if (kind === "create") {
                  const tag = String(o.tag ?? "div").toLowerCase().replace(/[^a-z0-9_-]/g, "");
                  if (!tag) throw new Error("create requires tag");
                  const el = document.createElement(tag);

                  const attrs = safeObject(o.attrs);
                  Object.keys(attrs).slice(0, 50).forEach((k) => {
                    const name = String(k);
                    const value = String(attrs[k] ?? "");
                    try {
                      el.setAttribute(name, value.slice(0, 2000));
                    } catch {
                      // ignore invalid attrs
                    }
                  });

                  if (o.text !== undefined) {
                    el.textContent = String(o.text ?? "").slice(0, 20000);
                  }
                  if (o.html !== undefined) {
                    const html = String(o.html ?? "");
                    el.innerHTML = html.length > 50000 ? html.slice(0, 50000) : html;
                  }

                  const parentSel = String(o.parent_selector ?? o.parent ?? "body");
                  const parent = (parentSel ? document.querySelector(parentSel) : document.body) ?? document.body;
                  const insert = String(o.insert ?? "append");
                  if (insert === "prepend" && "prepend" in parent) (parent as any).prepend(el);
                  else (parent as any).append(el);
                  return { op: "create", tag, parent_selector: parentSel, inserted: true };
                }

                const selector = String(o.selector ?? "");
                if (!selector) throw new Error(`${kind} requires selector`);
                const nodes = Array.from(document.querySelectorAll(selector)).slice(0, limit) as any[];

                if (kind === "remove") {
                  let removed = 0;
                  nodes.forEach((n) => {
                    try {
                      n.remove();
                      removed += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: "remove", selector, removed };
                }

                if (kind === "set_attr") {
                  const name = String(o.name ?? "");
                  const value = String(o.value ?? "");
                  if (!name) throw new Error("set_attr requires name");
                  let set = 0;
                  nodes.forEach((n) => {
                    try {
                      n.setAttribute(name, value.slice(0, 2000));
                      set += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: "set_attr", selector, name, set };
                }

                if (kind === "remove_attr") {
                  const name = String(o.name ?? "");
                  if (!name) throw new Error("remove_attr requires name");
                  let removed = 0;
                  nodes.forEach((n) => {
                    try {
                      n.removeAttribute(name);
                      removed += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: "remove_attr", selector, name, removed };
                }

                if (kind === "set_text") {
                  const text = String(o.text ?? "").slice(0, 20000);
                  let set = 0;
                  nodes.forEach((n) => {
                    try {
                      n.textContent = text;
                      set += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: "set_text", selector, set, bytes: text.length };
                }

                if (kind === "set_html" || kind === "append_html") {
                  const html = String(o.html ?? "");
                  const bounded = html.length > 50000 ? html.slice(0, 50000) : html;
                  let set = 0;
                  nodes.forEach((n) => {
                    try {
                      if (kind === "append_html") n.insertAdjacentHTML("beforeend", bounded);
                      else n.innerHTML = bounded;
                      set += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: kind, selector, set, bytes: bounded.length };
                }

                if (kind === "dispatch") {
                  const eventType = String(o.event ?? o.type ?? "click");
                  const init = safeObject(o.event_init ?? o.init);
                  const bubbles = init.bubbles !== undefined ? !!init.bubbles : true;
                  const cancelable = init.cancelable !== undefined ? !!init.cancelable : true;
                  let fired = 0;
                  nodes.forEach((n) => {
                    try {
                      let ev: Event;
                      if (["click", "mousedown", "mouseup", "mousemove"].includes(eventType)) {
                        ev = new MouseEvent(eventType, { bubbles, cancelable });
                      } else {
                        ev = new Event(eventType, { bubbles, cancelable });
                      }
                      n.dispatchEvent(ev);
                      fired += 1;
                    } catch {
                      // ignore
                    }
                  });
                  return { op: "dispatch", selector, event: eventType, fired };
                }

                throw new Error(`unsupported dom_apply op: ${kind}`);
              };

              for (const op of ops) {
                try {
                  results.push({ ok: true, ...applyOne(op) });
                } catch (e) {
                  results.push({ ok: false, error: String(e) });
                }
              }

              return { kind: "dom_apply", ops: results, applied: results.filter((r) => r && r.ok).length, total: results.length };
            };

            const makeEntityApply = (args: any) => {
              if (!onSceneApply) {
                throw new Error("entity_apply not supported (no scene handler)");
              }
              // Preferred shape: explicit ops array (create/update/delete/action/clear).
              if (Array.isArray(args?.ops)) {
                const hasClear = args.ops.some((op: any) => String(op?.op ?? op?.kind ?? "").trim() === "clear");
                if (hasClear) throw new Error("scene clear is disabled in WebUI");
                return onSceneApply(args.ops);
              }

              // Compatibility: accept an "entities" list (older/alternate schema).
              // Each entity becomes a create+update op bundle.
              if (Array.isArray(args?.entities)) {
                const ops: any[] = [];
                const ents = args.entities as any[];
                for (const ent of ents.slice(0, 50)) {
                  if (!ent || typeof ent !== "object") continue;
                  const id = safeTrunc(String(ent?.id ?? ""), 200);
                  const entityKind = safeTrunc(String(ent?.entity_kind ?? ent?.entityKind ?? ent?.type ?? ent?.kind ?? ""), 100);
                  if (!id || !entityKind) continue;
                  const title = typeof ent?.title === "string" ? safeTrunc(String(ent.title), 200) : undefined;
                  const props = safeObject(ent?.props ?? ent ?? {});
                  ops.push({ op: "create", id, entity_kind: entityKind, title, props });
                  // Optional: actions array is converted into generic action ops (data only).
                  const actions = Array.isArray(ent?.actions) ? ent.actions : [];
                  for (const a of actions.slice(0, 20)) {
                    const name = safeTrunc(String(a?.name ?? a?.action ?? a?.kind ?? ""), 80);
                    if (!name) continue;
                    ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? a ?? {}) });
                  }
                }
                return onSceneApply(ops);
              }

              // Compatibility: accept the older "id/type/props/actions" shorthand that many models
              // naturally invent even if they weren't given the exact ops schema.
              const id = safeTrunc(String(args?.id ?? ""), 200);
              const entityKind = safeTrunc(
                String(args?.entity_kind ?? args?.entityKind ?? args?.type ?? args?.kind ?? ""),
                100,
              );
              const props = safeObject(args?.props ?? {});
              const titleFromProps = typeof props?.title === "string" ? safeTrunc(String(props.title), 200) : "";
              const titleFromArgs = typeof args?.title === "string" ? safeTrunc(String(args.title), 200) : "";
              const title = titleFromArgs || titleFromProps || "";

              const ops: any[] = [];

              // Upsert/create.
              if (id && entityKind && (Object.keys(props).length > 0 || title)) {
                ops.push({
                  op: "create",
                  id,
                  entity_kind: entityKind,
                  title: title || undefined,
                  props,
                });
              } else if (id && Object.keys(props).length > 0) {
                // Patch/update (kind unknown).
                ops.push({ op: "update", id, props });
              }

              // Action(s).
              const actions = Array.isArray(args?.actions) ? args.actions : [];
              for (const a of actions.slice(0, 20)) {
                const name = safeTrunc(String(a?.name ?? a?.action ?? ""), 80);
                if (!name) continue;
                ops.push({ op: "action", id, action: name, args: safeObject(a?.args ?? {}) });
              }
              const singleAction = safeTrunc(String(args?.action ?? ""), 80);
              if (singleAction) {
                ops.push({ op: "action", id, action: singleAction, args: safeObject(args?.args ?? {}) });
              }

              // Delete/clear shorthands.
              if (args?.delete === true || args?.remove === true) {
                ops.push({ op: "delete", id });
              }
              if (args?.clear === true) {
                throw new Error("scene clear is disabled in WebUI");
              }

              return onSceneApply(ops);
            };

            const makeEntityQuery = (args: any) => {
              const ents = Array.isArray(sceneEntities) ? sceneEntities : [];
              const kind = typeof args?.entity_kind === "string" ? String(args.entity_kind).trim() : typeof args?.kind === "string" ? String(args.kind).trim() : "";
              const idPrefix = typeof args?.id_prefix === "string" ? String(args.id_prefix).trim() : "";
              const limit = clampInt(args?.limit, 1, 200, 50);
              const items = ents
                .filter((e: any) => (kind ? String(e?.kind ?? "") === kind : true))
                .filter((e: any) => (idPrefix ? String(e?.id ?? "").startsWith(idPrefix) : true))
                .slice(0, limit);
              return { kind: "entity_query", entity_kind: kind || undefined, id_prefix: idPrefix || undefined, count: items.length, items };
            };

            const makeMediaSnapshot = () => {
              const els = Array.from(document.querySelectorAll("audio,video")).slice(0, 20) as HTMLMediaElement[];
              const items = els.map((el) => {
                const isVideo = el.tagName.toLowerCase() === "video";
                const ds: any = (el as any).dataset || {};
                const src = el.currentSrc || el.src || "";
                const u = src ? tryParseUrl(src) : null;
                const fromFilePath = u && u.pathname.endsWith("/api/v1/file") ? u.searchParams.get("path") || "" : "";
                return {
                  kind: isVideo ? "video" : "audio",
                  tool_call_id: ds.toolCallId || undefined,
                  path: safeTrunc(String(ds.path || fromFilePath || ""), 200),
                  src: safeTrunc(String(src || ""), 300),
                  paused: el.paused,
                  ended: (el as any).ended,
                  current_time: Number.isFinite(el.currentTime) ? el.currentTime : 0,
                  duration: Number.isFinite(el.duration) ? el.duration : 0,
                };
              });
              return { kind: "media_snapshot", items };
            };

            const makeDomClick = (args: any) => {
              const selector = safeTrunc(String(args?.selector ?? ""), 200);
              if (!selector) throw new Error("dom_click requires selector");
              const el = document.querySelector(selector) as any;
              if (!el) return { kind: "dom_click", selector, clicked: false };
              if (typeof el.click === "function") {
                el.click();
                return { kind: "dom_click", selector, clicked: true, tag: String(el.tagName || "").toLowerCase() };
              }
              throw new Error("element has no click()");
            };

            const makeDomSetValue = (args: any) => {
              const selector = safeTrunc(String(args?.selector ?? ""), 200);
              const rawValue = String(args?.value ?? "");
              const value = rawValue.length > 2000 ? rawValue.slice(0, 2000) : rawValue;
              if (!selector) throw new Error("dom_set_value requires selector");
              const el = document.querySelector(selector) as any;
              if (!el) return { kind: "dom_set_value", selector, set: false };
              if (!("value" in el)) throw new Error("element has no value");
              el.value = value;
              const dispatch = args?.dispatch_events !== false;
              if (dispatch) {
                try {
                  el.dispatchEvent(new Event("input", { bubbles: true }));
                  el.dispatchEvent(new Event("change", { bubbles: true }));
                } catch {
                  // ignore
                }
              }
              return { kind: "dom_set_value", selector, set: true };
            };

            const makeArtifactUrl = async (args: any) => {
              const token = typeof daemonAuthToken === "string" ? daemonAuthToken.trim() : "";
              const sid = typeof sessionId === "string" ? sessionId.trim() : "";
              const path = safeTrunc(String(args?.path ?? args?.artifact?.path ?? ""), 4000).trim();
              const resolvedPath = safeTrunc(
                String(args?.resolved_path ?? args?.resolvedPath ?? args?.artifact?.resolved_path ?? ""),
                4000,
              ).trim();
              const wantYolo = typeof args?.yolo === "boolean" ? args.yolo : yolo;

              if (!path && !resolvedPath) throw new Error("artifact_url requires path or resolved_path");

              const tryPaths: string[] = [];
              if (path) tryPaths.push(path);
              // In YOLO mode, absolute paths are allowed on /api/v1/file. This provides a fallback when the
              // artifact payload includes a resolved absolute path.
              if (wantYolo && resolvedPath && !tryPaths.includes(resolvedPath)) tryPaths.push(resolvedPath);

              let lastErr: any = null;
              for (const p of tryPaths) {
                const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
                const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(p)}&yolo=${wantYolo ? "1" : "0"}${sidQ}`;
                try {
                  const r = await fetch(src, {
                    method: "GET",
                    headers: token ? { Authorization: `Bearer ${token}` } : {},
                  });
                  if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
                  const ct = String(r.headers.get("content-type") || "").trim();
                  const b = await r.blob();
                  const u = URL.createObjectURL(b);

                  artifactBlobUrlsRef.current.push(u);
                  while (artifactBlobUrlsRef.current.length > 32) {
                    const old = artifactBlobUrlsRef.current.shift();
                    if (!old) break;
                    try {
                      URL.revokeObjectURL(old);
                    } catch {
                      // ignore
                    }
                  }

                  return {
                    kind: "artifact_url",
                    ok: true,
                    url: u,
                    source_path: p,
                    content_type: ct || undefined,
                    size_bytes: typeof (b as any)?.size === "number" ? (b as any).size : undefined,
                  };
                } catch (e) {
                  lastErr = e;
                }
              }
              throw lastErr || new Error("artifact_url failed");
            };

            const makeMediaPlay = async (args: any) => {
              const urlRaw = String(args?.url ?? args?.src ?? "").trim();
              const pathRaw = String(args?.path ?? "").trim();
              const resolvedPathRaw = String(args?.resolved_path ?? args?.resolvedPath ?? "").trim();

              // Preferred: play an explicit selector (existing element).
              const selector = safeTrunc(String(args?.selector ?? ""), 200).trim();
              if (selector) {
                const el = document.querySelector(selector) as any;
                if (!el) return { kind: "media_play", selector, ok: false, error: "no element matched" };
                if (typeof el.play !== "function") return { kind: "media_play", selector, ok: false, error: "element has no play()" };
                try {
                  await el.play();
                  return { kind: "media_play", selector, ok: true };
                } catch (e) {
                  return { kind: "media_play", selector, ok: false, error: String(e) };
                }
              }

              // Convenience: if the agent provides a URL or artifact path, create (or reuse) an element and attempt play.
              // This is intentionally powerful; autoplay may still be blocked by browser gesture policies.
              const id = safeTrunc(String(args?.id ?? args?.element_id ?? ""), 80).trim();
              const tagRaw = String(args?.tag ?? args?.element ?? args?.kind ?? "").toLowerCase();
              const tag = tagRaw.includes("video") ? "video" : "audio";

              let url = urlRaw;
              if (!url && (pathRaw || resolvedPathRaw)) {
                const u = await makeArtifactUrl({ path: pathRaw, resolved_path: resolvedPathRaw, yolo: typeof args?.yolo === "boolean" ? args.yolo : yolo });
                url = String((u as any)?.url ?? "").trim();
              }
              if (!url) return { kind: "media_play", ok: false, error: "media_play requires selector or url/path" };

              let el: any = null;
              if (id) el = document.getElementById(id);
              if (!el) {
                el = document.createElement(tag);
                if (id) el.id = id;
                try {
                  document.body.appendChild(el);
                } catch {
                  // ignore
                }
              }
              if (!el) return { kind: "media_play", ok: false, error: "failed to create element" };

              // Set common properties in a bounded way.
              try {
                el.controls = args?.controls !== false;
              } catch {
                // ignore
              }
              try {
                if (typeof args?.autoplay === "boolean") el.autoplay = args.autoplay;
              } catch {
                // ignore
              }
              try {
                if (typeof args?.loop === "boolean") el.loop = args.loop;
              } catch {
                // ignore
              }
              try {
                if (typeof args?.muted === "boolean") el.muted = args.muted;
              } catch {
                // ignore
              }
              try {
                if (typeof args?.volume === "number" && Number.isFinite(args.volume)) el.volume = Math.min(1, Math.max(0, args.volume));
              } catch {
                // ignore
              }
              try {
                el.src = url;
              } catch {
                // ignore
              }

              try {
                await el.play();
                return { kind: "media_play", ok: true, created: true, tag, id: id || undefined, url: safeTrunc(url, 300) };
              } catch (e) {
                return { kind: "media_play", ok: false, created: true, tag, id: id || undefined, url: safeTrunc(url, 300), error: String(e) };
              }
            };

            const makeLocation = () => {
              const href = String(window?.location?.href ?? "");
              const u = href ? tryParseUrl(href) : null;
              const sp = u ? u.searchParams : null;
              const searchParams: any = {};
              let hasSensitive = false;
              if (sp) {
                const keys = Array.from(sp.keys()).slice(0, 40);
                keys.forEach((k) => {
                  const v = sp.get(k);
                  const sensitive = isSensitiveKey(k);
                  if (sensitive) hasSensitive = true;
                  searchParams[k] = sensitive ? "(redacted)" : safeTrunc(String(v ?? ""), 200);
                });
              }
              return {
                kind: "location",
                href: safeTrunc(href, 400),
                origin: safeTrunc(String(window?.location?.origin ?? ""), 200),
                pathname: safeTrunc(String(window?.location?.pathname ?? ""), 200),
                search: safeTrunc(String(window?.location?.search ?? ""), 200),
                hash: safeTrunc(String(window?.location?.hash ?? ""), 200),
                search_params: searchParams,
                has_sensitive_query: hasSensitive,
                title: safeTrunc(String(document?.title ?? ""), 200),
              };
            };

            const makeMediaObserve = async (args: any) => {
              const eventsRaw = Array.isArray(args?.events) ? args.events : ["play", "pause", "ended", "error"];
              const allowed = new Set(["play", "pause", "ended", "error", "timeupdate"]);
              const events: string[] = eventsRaw
                .map((x: any) => String(x))
                .filter((x: string) => allowed.has(x))
                .slice(0, 5);
              const selectorFromArgs = safeTrunc(String(args?.selector ?? ""), 200);
              const toolId = safeTrunc(String(args?.tool_call_id ?? ""), 120);
              // Prefer matching by tool_call_id (artifact media elements set data-tool-call-id).
              // Fall back to selector, then to all audio/video.
              // eslint-disable-next-line @typescript-eslint/no-explicit-any
              const g: any = typeof globalThis !== "undefined" ? globalThis : {};
              const cssEscape = typeof g?.CSS?.escape === "function" ? g.CSS.escape : (s: string) => s.replace(/[^a-zA-Z0-9_-]/g, "");
              const sel =
                toolId.length > 0
                  ? `audio[data-tool-call-id="${cssEscape(toolId)}"],video[data-tool-call-id="${cssEscape(toolId)}"]`
                  : selectorFromArgs.length > 0
                    ? selectorFromArgs
                    : "audio,video";

              const els = Array.from(document.querySelectorAll(sel)).slice(0, 10) as HTMLMediaElement[];
              if (els.length === 0) return { kind: "media_observe", selector: sel, observing: 0 };

              const mkPayload = (el: HTMLMediaElement) => ({
                kind: el.tagName.toLowerCase() === "video" ? "video" : "audio",
                current_time: Number.isFinite(el.currentTime) ? el.currentTime : 0,
                duration: Number.isFinite(el.duration) ? el.duration : 0,
                paused: !!el.paused,
                ended: !!(el as any).ended,
              });

              // Attach listeners (idempotent per rpc_id).
              if (!rpcCleanupRef.current[rpcId]) {
                const cleanups: Array<() => void> = [];
                els.forEach((el) => {
                  events.forEach((evName) => {
                    const handler = () => {
                      void postRpcProgress(evName, mkPayload(el)).catch(() => {});
                    };
                    el.addEventListener(evName, handler);
                    cleanups.push(() => {
                      try {
                        el.removeEventListener(evName, handler);
                      } catch {
                        // ignore
                      }
                    });
                  });
                });
                rpcCleanupRef.current[rpcId] = cleanups;
              }

              // Emit an initial snapshot as progress so agents can reason without waiting for a change.
              await postRpcProgress("attached", { selector: sel, observing: els.length });
              els.forEach((el) => void postRpcProgress("snapshot", mkPayload(el)).catch(() => {}));
              return { kind: "media_observe", selector: sel, observing: els.length, events };
            };

            const makeStateSnapshot = () => {
              const loc = makeLocation();
              const media = makeMediaSnapshot();
              return { kind: "state_snapshot", location: loc, media: media.items ?? [] };
            };

            const makeNavigate = (args: any) => {
              const url = safeTrunc(String(args?.url ?? args?.href ?? ""), 2000);
              if (!url) throw new Error("navigate requires url");
              // This is intentionally side-effecting and may reload the page.
              window.location.assign(url);
              return { kind: "navigate", url };
            };

            const makeScriptEval = async (args: any) => {
              const code = String(args?.code ?? "");
              if (!code) throw new Error("script_eval requires code");

              const timeoutMs = clampInt(args?.timeout_ms ?? args?.timeoutMs, 50, 60000, 8000);
              const userArgs = typeof args?.args === "object" && args?.args ? args.args : {};

              const workerSource = `
                "use strict";
                // Best-effort hardening: remove common network primitives.
                try { self.fetch = undefined; } catch {}
                try { self.WebSocket = undefined; } catch {}
                try { self.importScripts = undefined; } catch {}

                const pending = new Map();
                let seq = 1;

                function call(method, args) {
                  return new Promise((resolve, reject) => {
                    const id = seq++;
                    pending.set(id, { resolve, reject });
                    self.postMessage({ type: "call", id, method, args });
                  });
                }

                const api = {
                  call,
                  progress: (name, payload) => call("rpc.progress", { name, payload }),
                  dom: {
                    query: (q) => call("dom.query", q || {}),
                    click: (q) => call("dom.click", q || {}),
                    setValue: (q) => call("dom.set_value", q || {}),
                    apply: (q) => call("dom.apply", q || {}),
                  },
                  scene: {
                    apply: (q) => call("scene.apply", q || {}),
                    query: (q) => call("scene.query", q || {}),
                    create: (kind, props, title) =>
                      call("scene.apply", { ops: [{ op: "create", entity_kind: String(kind || ""), title: title || undefined, props: props || {} }] }),
                    update: (id, patch) => call("scene.apply", { ops: [{ op: "update", id: String(id || ""), props: patch || {} }] }),
                    remove: (id) => call("scene.apply", { ops: [{ op: "delete", id: String(id || "") }] }),
                    action: (id, action, a) =>
                      call("scene.apply", { ops: [{ op: "action", id: String(id || ""), action: String(action || ""), args: a || {} }] }),
                  },
                  media: {
                    snapshot: () => call("media.snapshot", {}),
                    play: (q) => call("media.play", q || {}),
                    observe: (q) => call("media.observe", q || {}),
                  },
                  location: {
                    get: () => call("location.get", {}),
                  },
                  nav: {
                    go: (q) => call("nav.go", q || {}),
                  },
                  sleep: (ms) => new Promise((r) => setTimeout(r, Math.max(0, Number(ms) || 0))),
                };

                self.onmessage = async (evt) => {
                  const msg = evt && evt.data ? evt.data : {};
                  if (msg.type === "resp") {
                    const p = pending.get(msg.id);
                    if (!p) return;
                    pending.delete(msg.id);
                    if (msg.ok) p.resolve(msg.result);
                    else p.reject(new Error(String(msg.error || "call failed")));
                    return;
                  }
                  if (msg.type !== "run") return;
                  const runId = String(msg.run_id || "");
                  const code = String(msg.code || "");
                  const args = msg.args || {};
                  try {
                    // Execute user code as an async IIFE so it can use await.
                    // Note: if user code blocks the worker event loop, the host must terminate the worker.
                    const fn = new Function("api", "args", '"use strict"; return (async function() {\\n' + code + '\\n})();');
                    const result = await fn(api, args);
                    self.postMessage({ type: "done", run_id: runId, ok: true, result });
                  } catch (e) {
                    self.postMessage({ type: "done", run_id: runId, ok: false, error: String(e) });
                  }
                };
              `;

              const worker = createInlineWorker(workerSource);
              const runId = `${rpcId}::${Date.now()}::${Math.random().toString(16).slice(2)}`;
              let finished = false;

              const respond = (id: number, ok: boolean, payload: any) => {
                worker.postMessage({ type: "resp", id, ok, ...(ok ? { result: payload } : { error: payload }) });
              };

              const donePromise = new Promise<any>((resolve, reject) => {
                worker.onmessage = (evt) => {
                  const msg: any = evt && evt.data ? evt.data : {};
                  if (msg.type === "call") {
                    const id = Number(msg.id);
                    const method = String(msg.method || "");
                    const cargs = msg.args ?? {};
                    void (async () => {
                      try {
                        const sideEffectMethods = new Set([
                          "dom.click",
                          "dom.set_value",
                          "dom.apply",
                          "scene.apply",
                          "media.play",
                          "media.observe",
                          "nav.go",
                        ]);
                        if (sideEffectMethods.has(method) && !allowClientEffects) {
                          throw new Error("side effects disabled by settings");
                        }
                        let out: any = null;
                        if (method === "dom.query") out = makeDomQuery(cargs);
                        else if (method === "dom.click") out = makeDomClick(cargs);
                        else if (method === "dom.set_value") out = makeDomSetValue(cargs);
                        else if (method === "dom.apply") out = makeDomApply(cargs);
                        else if (method === "scene.apply") out = makeEntityApply(cargs);
                        else if (method === "scene.query") out = makeEntityQuery(cargs);
                        else if (method === "media.snapshot") out = makeMediaSnapshot();
                        else if (method === "media.play") out = await makeMediaPlay(cargs);
                        else if (method === "media.observe") out = await makeMediaObserve(cargs);
                        else if (method === "location.get") out = makeLocation();
                        else if (method === "nav.go") out = makeNavigate(cargs);
                        else if (method === "rpc.progress") {
                          const name = String(cargs?.name ?? "progress");
                          await postRpcProgress(name, cargs?.payload ?? {});
                          out = true;
                        } else {
                          throw new Error(`unsupported script api method: ${method}`);
                        }
                        respond(id, true, out);
                      } catch (e) {
                        respond(id, false, String(e));
                      }
                    })();
                    return;
                  }
                  if (msg.type === "done" && String(msg.run_id || "") === runId) {
                    finished = true;
                    if (msg.ok) resolve(msg.result);
                    else reject(new Error(String(msg.error || "script failed")));
                  }
                };
                worker.onerror = (e) => {
                  if (finished) return;
                  reject(new Error(`worker error: ${String((e as any)?.message ?? e)}`));
                };
              });

              const timeoutPromise = new Promise<never>((_, reject) => {
                setTimeout(() => reject(new Error(`script timeout after ${timeoutMs}ms`)), timeoutMs);
              });

              try {
                worker.postMessage({ type: "run", run_id: runId, code, args: userArgs });
                const result = await Promise.race([donePromise, timeoutPromise]);
                return { kind: "script_eval", ok: true, timeout_ms: timeoutMs, result };
              } finally {
                try {
                  worker.terminate();
                } catch {
                  // ignore
                }
              }
            };

            const makePageEval = async (args: any) => {
              if (!allowUnsafePageEval) throw new Error("page_eval disabled by settings");
              const code = String(args?.code ?? "");
              if (!code) throw new Error("page_eval requires code");

              const timeoutMs = clampInt(args?.timeout_ms ?? args?.timeoutMs, 50, 60000, 8000);
              const userArgs = typeof args?.args === "object" && args?.args ? args.args : {};

              const api = {
                progress: async (name: string, payload?: any) => {
                  await postRpcProgress(name, payload ?? {});
                  return true;
                },
                dom: {
                  query: async (q: any) => makeDomQuery(q || {}),
                  click: async (q: any) => makeDomClick(q || {}),
                  setValue: async (q: any) => makeDomSetValue(q || {}),
                  apply: async (q: any) => makeDomApply(q || {}),
                },
                scene: {
                  apply: async (q: any) => makeEntityApply(q || {}),
                  query: async (q: any) => makeEntityQuery(q || {}),
                },
                media: {
                  snapshot: async () => makeMediaSnapshot(),
                  play: async (q: any) => makeMediaPlay(q || {}),
                  observe: async (q: any) => makeMediaObserve(q || {}),
                },
                location: {
                  get: async () => makeLocation(),
                },
                nav: {
                  go: async (q: any) => makeNavigate(q || {}),
                },
                sleep: async (ms: any) => new Promise((r) => setTimeout(r, Math.max(0, Number(ms) || 0))),
              };

              // WARNING: This executes on the browser main thread and cannot preempt infinite loops.
              // Prefer `script_eval` in a worker when possible.
              const runPromise = (async () => {
                // eslint-disable-next-line no-new-func
                const fn = new Function(
                  "api",
                  "args",
                  // Use an async function expression (not an async arrow) for maximum browser compatibility.
                  '"use strict"; return (async function() {\n' + code + "\n})();",
                ) as (api: any, args: any) => Promise<any>;
                return await fn(api, userArgs);
              })();

              const timeoutPromise = new Promise<never>((_, reject) => {
                setTimeout(() => reject(new Error(`page_eval timeout after ${timeoutMs}ms`)), timeoutMs);
              });

              const result = await Promise.race([runPromise, timeoutPromise]);
              return { kind: "page_eval", ok: true, timeout_ms: timeoutMs, result };
            };

            let result: any = null;
            if (rpcKind === "dom_query") result = makeDomQuery(rpcArgs);
            else if (rpcKind === "dom_apply") result = makeDomApply(rpcArgs);
            else if (rpcKind === "entity_apply") result = makeEntityApply(rpcArgs);
            else if (rpcKind === "entity_query") result = makeEntityQuery(rpcArgs);
            else if (rpcKind === "media_snapshot") result = makeMediaSnapshot();
            else if (rpcKind === "location") result = makeLocation();
            else if (rpcKind === "state_snapshot") result = makeStateSnapshot();
            else if (rpcKind === "dom_click") result = makeDomClick(rpcArgs);
            else if (rpcKind === "dom_set_value") result = makeDomSetValue(rpcArgs);
            else if (rpcKind === "media_play") result = await makeMediaPlay(rpcArgs);
            else if (rpcKind === "media_observe") result = await makeMediaObserve(rpcArgs);
            else if (rpcKind === "navigate") result = makeNavigate(rpcArgs);
            else if (rpcKind === "artifact_url") result = await makeArtifactUrl(rpcArgs);
            else if (rpcKind === "script_eval") result = await makeScriptEval(rpcArgs);
            else if (rpcKind === "page_eval") result = await makePageEval(rpcArgs);
            else throw new Error(`unsupported rpc.kind: ${rpcKind}`);

            await postRpcResult(true, { elapsed_ms: Date.now() - t0, result });
          } catch (e) {
            await postRpcResult(false, { elapsed_ms: Date.now() - t0, error: String(e) });
          }
        };

        // Key auto-run de-duping by tool_call_id first (unique per request).
        // Some agents may reuse rpc_id strings across multiple requests (e.g. "get_artifact_url").
        // If we de-dupe by rpc_id first, later requests would never auto-run, causing client_wait_event timeouts.
        const ackKey = `rpc:${toolCallId || rpcId || idx}`;
        const sidKey = typeof sessionId === "string" ? sessionId.trim() : "";
        const globalKey = `${sidKey || "no_session"}::${ackKey}`;
        const globalOnce = globalAutoRunOnceMap();
        const alreadyRan = !!probeRanRef.current[ackKey] || !!globalOnce[globalKey];
        // Avoid running entity_apply during render; it is handled in an effect so Scene updates reliably.
        // Also allow callers (e.g. history views) to disable auto-running client RPCs entirely.
        if (!disableAutoClientRpcs && autoRun && rpcKind !== "entity_apply" && !alreadyRan && canRun) {
          probeRanRef.current[ackKey] = true;
          globalOnce[globalKey] = true;
          pendingAutoRunsRef.current[globalKey] = () => {
            void runRpc().catch(() => {});
          };
        }

        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
            <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
              Client RPC requested: <code>{rpcKind || "(missing kind)"}</code>
              {rpcId ? (
                <span className="ml-2 text-[11px] text-white/50">
                  rpc_id=<code>{rpcId}</code>
                </span>
              ) : null}
            </div>
            <details className="mt-2 rounded-md border border-white/10 bg-black/20 px-3 py-2">
              <summary className="cursor-pointer select-none text-[11px] font-semibold text-white/70">Request payload</summary>
              <pre className="mt-2 overflow-auto whitespace-pre-wrap font-mono text-[11px] leading-relaxed text-white/90">
                {JSON.stringify(
                  {
                    type: atype,
                    tool_call_id: toolCallId || undefined,
                    rpc_id: rpcId,
                    rpc: { kind: rpcKind, args: rpcArgs },
                    side_effects: sideEffectsRequested,
                    auto_run: autoRunRequested,
                  },
                  null,
                  2,
                )}
              </pre>
            </details>
            <div className="mt-2 flex flex-wrap items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canRun || !allowClientRpcs || (sideEffectsRequested && !allowClientEffects)}
                title={
                  !allowClientRpcs
                    ? "Enable “Allow agent-requested client RPCs” in settings to run"
                    : sideEffectsRequested && !allowClientEffects
                      ? "Enable “Allow agent-requested client RPCs with side effects” in settings to run"
                      : ""
                }
                onClick={() => {
                  probeRanRef.current[ackKey] = true;
                  void runRpc().catch(() => {});
                }}
              >
                Run RPC
              </button>
              {!allowClientRpcs ? (
                <div className="text-[11px] text-white/40">Disabled by settings</div>
              ) : sideEffectsRequested && !allowClientEffects ? (
                <div className="text-[11px] text-white/40">Side effects disabled</div>
              ) : null}
            </div>
          </Card>,
        );
        return;
      }

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

        items.push(
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
                      setAckedKeys((prev) => ({ ...prev, [ackKey]: true }));
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
          </Card>,
        );
        return;
      }

      if (atype === "notify") {
        const msg = String(action?.message ?? "");
        const ackKey = toolCallId ? `tool_call:${toolCallId}` : `notify:${title}:${msg}`;
        const canAck = typeof sessionId === "string" && sessionId.trim().length > 0;
        items.push(
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
                      setAckedKeys((prev) => ({ ...prev, [ackKey]: true }));
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
          </Card>,
        );
        return;
      }

      items.push(
        <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {JSON.stringify(action, null, 2)}
          </pre>
        </Card>,
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
