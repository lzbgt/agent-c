import React from "react";
import { apiPostSessionUiEvent, type AgentEvent } from "../api";
import Markdown from "./Markdown";
import ToolResultView from "./ToolResultView";
import ArtifactView from "./ArtifactView";

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
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">{title}</div>
      </div>
      <div className="px-3 pb-3">{children}</div>
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
  sceneEntities?: any[];
  onSceneApply?: (ops: any[]) => any;
}) {
  const [ackError, setAckError] = React.useState<string | null>(null);
  const [ackedKeys, setAckedKeys] = React.useState<Record<string, boolean>>({});
  const shownUiActionRef = React.useRef<Record<string, boolean>>({});

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

  const items: Array<React.ReactNode> = [];
  let streamedAssistant = "";
  let sawFinalAssistant = false;
  let sawToolOrAssistant = false;
  let lastHeartbeat: any = null;

  const probeRanRef = React.useRef<Record<string, boolean>>({});
  const rpcCleanupRef = React.useRef<Record<string, Array<() => void>>>({});

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
      const args = typeof data.arguments_json === "string" ? data.arguments_json : "";
      items.push(
        <Card key={`tc-${idx}`} title={`Tool call: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {args ? prettyJsonOrRaw(args) : "(enable verbose to capture arguments)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "tool_result") {
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      if (typeof data.content === "string") {
        items.push(
          <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
            <ToolResultView baseUrl={baseUrl} yolo={yolo} content={data.content} />
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
          "artifact_play",
          "navigate",
          "page_eval",
        ]);
        const sideEffectsRequested = rpc?.side_effects === true || action?.side_effects === true || sideEffectKinds.has(rpcKind);

        const canRun = !!rpcId && typeof sessionId === "string" && sessionId.trim().length > 0;
        const canRunAuto = !!allowClientRpcs && (!sideEffectsRequested || !!allowClientEffects);
        const autoRun = canRunAuto && (action?.auto_run === true || action?.auto === true);

        const postRpcResult = async (ok: boolean, payload: any) => {
          const base = {
            rpc_id: rpcId,
            request_tool_call_id: toolCallId,
            rpc_kind: rpcKind,
            ok,
            elapsed_ms: payload?.elapsed_ms,
          };
          const data = ok ? { ...base, result: payload?.result } : { ...base, error: payload?.error };
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
            const legacyData = ok ? { ...legacyBase, result: payload?.result } : { ...legacyBase, error: payload?.error };
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
          if (!canRun) throw new Error("missing session/rpc_id");
          if (!rpcKind) throw new Error("missing rpc.kind");
          if (!allowClientRpcs) throw new Error("client RPC disabled by settings");
          if (sideEffectsRequested && !allowClientEffects) throw new Error("client RPC side effects disabled by settings");
          const t0 = Date.now();
          try {
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
              const ops = Array.isArray(args?.ops) ? args.ops : [];
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

            const makeMediaPlay = async (args: any) => {
              const selector = safeTrunc(String(args?.selector ?? "audio,video"), 200);
              const el = document.querySelector(selector) as any;
              if (!el) return { kind: "media_play", selector, ok: false, error: "no element matched" };
              if (typeof el.play !== "function") return { kind: "media_play", selector, ok: false, error: "element has no play()" };
              try {
                await el.play();
                return { kind: "media_play", selector, ok: true };
              } catch (e) {
                return { kind: "media_play", selector, ok: false, error: String(e) };
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
                    clear: (entityKind) => call("scene.apply", { ops: [{ op: "clear", entity_kind: entityKind || "" }] }),
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
                    const fn = new Function("api", "args", '"use strict"; return (async () => {\\n' + code + '\\n})();');
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
                  '"use strict"; return (async () => {\\n' + code + "\\n})();",
                ) as (api: any, args: any) => Promise<any>;
                return await fn(api, userArgs);
              })();

              const timeoutPromise = new Promise<never>((_, reject) => {
                setTimeout(() => reject(new Error(`page_eval timeout after ${timeoutMs}ms`)), timeoutMs);
              });

              const result = await Promise.race([runPromise, timeoutPromise]);
              return { kind: "page_eval", ok: true, timeout_ms: timeoutMs, result };
            };

            const makeArtifactPlay = async (args: any) => {
              const path = safeTrunc(String(args?.path ?? ""), 400);
              if (!path) throw new Error("artifact_play requires path");
              const kindHint = String(args?.kind ?? args?.media_kind ?? "").toLowerCase().trim();
              const repeat = clampInt(args?.repeat, 1, 16, 1);
              const autoplay = args?.autoplay !== undefined ? !!args.autoplay : true;
              const waitFor = String(args?.wait_for ?? "started").toLowerCase();
              const timeoutMs = clampInt(args?.timeout_ms ?? args?.timeoutMs, 50, 60000, 60000);

              const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(path)}&yolo=${yolo ? "1" : "0"}`;
              const isVideo = kindHint === "video" || /\.(mp4|webm|mov)$/i.test(path);

              // Replace any previous playback for this rpc_id (idempotent per rpc_id).
              if (rpcCleanupRef.current[rpcId]) {
                rpcCleanupRef.current[rpcId].forEach((fn) => {
                  try {
                    fn();
                  } catch {
                    // ignore
                  }
                });
                delete rpcCleanupRef.current[rpcId];
              }

              let remaining = repeat;
              let started = false;
              let finished = false;

              const el: HTMLMediaElement = isVideo ? document.createElement("video") : document.createElement("audio");
              el.src = src;
              el.preload = "auto";
              try {
                (el as any).dataset.rpcId = rpcId;
                if (toolCallId) (el as any).dataset.toolCallId = toolCallId;
                (el as any).dataset.path = path;
              } catch {
                // ignore
              }

              // Attach video elements so playback is more likely (and so it can be inspected via DOM).
              if (isVideo) {
                const v = el as HTMLVideoElement;
                v.muted = true; // best-effort autoplay aid
                v.playsInline = true;
                v.style.position = "fixed";
                v.style.left = "-9999px";
                v.style.top = "0";
                v.style.width = "1px";
                v.style.height = "1px";
                document.body.appendChild(v);
              }

              const stop = () => {
                try {
                  el.pause();
                } catch {
                  // ignore
                }
                try {
                  el.removeAttribute("src");
                  (el as any).load?.();
                } catch {
                  // ignore
                }
                try {
                  if (isVideo) (el as any).remove?.();
                } catch {
                  // ignore
                }
              };

              const onEnded = () => {
                if (finished) return;
                void postRpcProgress("ended", {
                  path,
                  kind: isVideo ? "video" : "audio",
                  current_time: Number.isFinite((el as any).currentTime) ? (el as any).currentTime : 0,
                  duration: Number.isFinite((el as any).duration) ? (el as any).duration : 0,
                  remaining: Math.max(0, remaining - 1),
                }).catch(() => {});
                if (remaining <= 1) {
                  finished = true;
                  stop();
                  void postRpcProgress("finished", { path, kind: isVideo ? "video" : "audio", repeat }).catch(() => {});
                  return;
                }
                remaining -= 1;
                try {
                  (el as any).currentTime = 0;
                } catch {
                  // ignore
                }
                void (el as any)
                  .play()
                  .then(() => {})
                  .catch((e: any) => {
                    finished = true;
                    stop();
                    void postRpcProgress("failed", { path, error: String(e), kind: isVideo ? "video" : "audio" }).catch(() => {});
                  });
              };

              const onError = (e: any) => {
                if (finished) return;
                finished = true;
                stop();
                void postRpcProgress("failed", { path, error: String(e?.message ?? "media error"), kind: isVideo ? "video" : "audio" }).catch(
                  () => {},
                );
              };

              el.addEventListener("ended", onEnded);
              el.addEventListener("error", onError as any);
              rpcCleanupRef.current[rpcId] = [
                () => el.removeEventListener("ended", onEnded),
                () => el.removeEventListener("error", onError as any),
                stop,
              ];

              if (!autoplay) {
                await postRpcProgress("ready", { path, kind: isVideo ? "video" : "audio", repeat }).catch(() => {});
                return { kind: "artifact_play", ok: true, autoplay: false, repeat };
              }

              try {
                await (el as any).play();
                started = true;
                await postRpcProgress("started", { path, kind: isVideo ? "video" : "audio", repeat }).catch(() => {});
              } catch (e) {
                stop();
                return { kind: "artifact_play", ok: false, error: String(e), autoplay: true, repeat };
              }

              if (waitFor === "finished" || waitFor === "ended") {
                await new Promise<void>((resolve, reject) => {
                  const t0 = Date.now();
                  const interval = setInterval(() => {
                    const elapsed = Date.now() - t0;
                    if (elapsed > timeoutMs) {
                      clearInterval(interval);
                      reject(new Error(`artifact_play wait_for=${waitFor} timed out after ${timeoutMs}ms`));
                      return;
                    }
                    if (waitFor === "finished" && finished) {
                      clearInterval(interval);
                      resolve();
                      return;
                    }
                    if (waitFor === "ended" && started && (el as any).ended) {
                      clearInterval(interval);
                      resolve();
                    }
                  }, 50);
                });
              }

              return { kind: "artifact_play", ok: true, autoplay: true, repeat };
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
            else if (rpcKind === "script_eval") result = await makeScriptEval(rpcArgs);
            else if (rpcKind === "page_eval") result = await makePageEval(rpcArgs);
            else if (rpcKind === "artifact_play") result = await makeArtifactPlay(rpcArgs);
            else throw new Error(`unsupported rpc.kind: ${rpcKind}`);

            await postRpcResult(true, { elapsed_ms: Date.now() - t0, result });
          } catch (e) {
            await postRpcResult(false, { elapsed_ms: Date.now() - t0, error: String(e) });
          }
        };

        const ackKey = `rpc:${rpcId || toolCallId || idx}`;
        const alreadyRan = !!probeRanRef.current[ackKey];
        if (autoRun && !alreadyRan && canRun) {
          probeRanRef.current[ackKey] = true;
          void runRpc().catch(() => {});
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
      if (atype === "play_audio") {
        const path = String(action?.path ?? "");
        const artifact = {
          path,
          kind: "audio",
          mime: action?.mime,
          title,
          autoplay: action?.autoplay,
          repeat: action?.repeat,
          source_tool_call_id: toolCallId,
        };
        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
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
