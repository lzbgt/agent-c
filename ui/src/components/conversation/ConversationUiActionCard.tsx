import React from "react";
import { daemonHeaders, type ApiAuth } from "../../api";
import ConversationCard from "../ConversationCard";
import {
  clampInt,
  createInlineWorker,
  globalAutoRunOnceMap,
  isSensitiveKey,
  safeObject,
  safeTrunc,
  tryParseUrl,
} from "./utils";

const Card = ConversationCard;

export type RpcCleanupEntry = {
  cleanups: Array<() => void>;
  kind: string;
  createdMs: number;
  lastActiveMs: number;
};

export type ConversationRpcRuntime = {
  pendingAutoRunsRef: React.MutableRefObject<Record<string, () => void>>;
  probeRanRef: React.MutableRefObject<Record<string, number>>;
  rpcCleanupRef: React.MutableRefObject<Record<string, RpcCleanupEntry>>;
  artifactBlobUrlsRef: React.MutableRefObject<string[]>;
  cleanupRpcEntry: (id: string) => boolean;
  markSeenWithLimit: (store: Record<string, number>, key: string, limit: number) => void;
  localUiActionLimit: number;
};

export type ConversationUiActionCardProps = {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  disableAutoClientRpcs?: boolean;
  data: any;
  idx: number;
  ackedKeys: Record<string, number>;
  ackError: string | null;
  setAckError: (msg: string | null) => void;
  markAckedKey: (key: string) => void;
  postClientEvent: (type: string, payload: any) => Promise<void>;
  runtime: ConversationRpcRuntime;
  sceneEntities?: any[];
  onSceneApply?: (ops: any[]) => any;
};

export default function ConversationUiActionCard({
  baseUrl,
  yolo,
  sessionId,
  daemonAuth,
  allowClientRpcs,
  allowClientEffects,
  allowUnsafePageEval,
  disableAutoClientRpcs,
  data,
  idx,
  ackedKeys,
  ackError,
  setAckError,
  markAckedKey,
  postClientEvent,
  runtime,
  sceneEntities,
  onSceneApply,
}: ConversationUiActionCardProps) {
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
      "open_url",
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
          const entityKind = safeTrunc(String(args?.entity_kind ?? args?.entityKind ?? args?.type ?? args?.kind ?? ""), 100);
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
          const kind =
            typeof args?.entity_kind === "string"
              ? String(args.entity_kind).trim()
              : typeof args?.kind === "string"
                ? String(args.kind).trim()
                : "";
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
          const hdr = daemonHeaders(daemonAuth);
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
                headers: hdr,
              });
              if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
              const ct = String(r.headers.get("content-type") || "").trim();
              const b = await r.blob();
              const u = URL.createObjectURL(b);

              runtime.artifactBlobUrlsRef.current.push(u);
              while (runtime.artifactBlobUrlsRef.current.length > 32) {
                const old = runtime.artifactBlobUrlsRef.current.shift();
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
            const u = await makeArtifactUrl({
              path: pathRaw,
              resolved_path: resolvedPathRaw,
              yolo: typeof args?.yolo === "boolean" ? args.yolo : yolo,
            });
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
          if (!runtime.rpcCleanupRef.current[rpcId]) {
            const cleanups: Array<() => void> = [];
            els.forEach((el) => {
              events.forEach((evName) => {
                const handler = () => {
                  const entry = runtime.rpcCleanupRef.current[rpcId];
                  if (entry) entry.lastActiveMs = Date.now();
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
            const now = Date.now();
            runtime.rpcCleanupRef.current[rpcId] = {
              cleanups,
              kind: "media_observe",
              createdMs: now,
              lastActiveMs: now,
            };
          }

          // Emit an initial snapshot as progress so agents can reason without waiting for a change.
          await postRpcProgress("attached", { selector: sel, observing: els.length });
          els.forEach((el) => void postRpcProgress("snapshot", mkPayload(el)).catch(() => {}));
          return { kind: "media_observe", selector: sel, observing: els.length, events };
        };

        const makeMediaUnobserve = (args: any) => {
          const ids: string[] = [];
          const addId = (v: any) => {
            const s = String(v ?? "").trim();
            if (s) ids.push(s);
          };
          const list = Array.isArray(args?.rpc_ids)
            ? args.rpc_ids
            : Array.isArray(args?.rpcIds)
              ? args.rpcIds
              : Array.isArray(args?.ids)
                ? args.ids
                : [];
          list.forEach(addId);
          addId(args?.rpc_id ?? args?.rpcId ?? args?.target_rpc_id ?? args?.targetRpcId ?? args?.tool_call_id ?? args?.toolCallId ?? args?.id);

          const entries = runtime.rpcCleanupRef.current;
          const unique = Array.from(new Set(ids));
          let targets = unique;
          if (args?.all === true || (targets.length === 0 && args?.all === "true")) {
            targets = Object.keys(entries).filter((id) => entries[id]?.kind === "media_observe");
          }

          let removed = 0;
          const removedIds: string[] = [];
          targets.forEach((id) => {
            const entry = entries[id];
            if (!entry || entry.kind !== "media_observe") return;
            if (runtime.cleanupRpcEntry(id)) {
              removed += 1;
              removedIds.push(id);
            }
          });

          const remaining = Object.keys(entries).filter((id) => entries[id]?.kind === "media_observe").length;
          return { kind: "media_unobserve", removed, removed_ids: removedIds, remaining };
        };

        const makeStateSnapshot = () => {
          const loc = makeLocation();
          const media = makeMediaSnapshot();
          return { kind: "state_snapshot", location: loc, media: media.items ?? [] };
        };

        const makeNavigate = (args: any) => {
          const url = safeTrunc(String(args?.url ?? args?.href ?? ""), 2000);
          if (!url) throw new Error("navigate requires url");
          const parsed = tryParseUrl(url);
          if (parsed && parsed.origin !== window.location.origin) {
            throw new Error("navigate only supports same-origin URLs; use open_url for external links");
          }
          // This is intentionally side-effecting and may reload the page.
          window.location.assign(url);
          return { kind: "navigate", url };
        };

        const makeOpenUrl = (args: any) => {
          const urlRaw = safeTrunc(String(args?.url ?? args?.href ?? ""), 2000);
          if (!urlRaw) throw new Error("open_url requires url");
          const parsed = tryParseUrl(urlRaw);
          if (!parsed) throw new Error("open_url requires absolute http(s) URL");
          if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
            throw new Error("open_url only supports http(s) URLs");
          }
          const label = safeTrunc(String(args?.title ?? ""), 120);
          const prompt = label ? `Open link?\n${label}\n${parsed.toString()}` : `Open link?\n${parsed.toString()}`;
          if (typeof window !== "undefined") {
            if (!window.confirm(prompt)) throw new Error("user declined");
            window.open(parsed.toString(), "_blank", "noopener,noreferrer");
          }
          return { kind: "open_url", url: parsed.toString(), opened: true };
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
                unobserve: (q) => call("media.unobserve", q || {}),
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
                      "nav.open",
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
                    else if (method === "media.unobserve") out = makeMediaUnobserve(cargs);
                    else if (method === "location.get") out = makeLocation();
                    else if (method === "nav.go") out = makeNavigate(cargs);
                    else if (method === "nav.open") out = makeOpenUrl(cargs);
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
              unobserve: async (q: any) => makeMediaUnobserve(q || {}),
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
              '"use strict"; return (async function() {\\n' + code + "\\n})();",
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
        else if (rpcKind === "media_unobserve") result = makeMediaUnobserve(rpcArgs);
        else if (rpcKind === "navigate") result = makeNavigate(rpcArgs);
        else if (rpcKind === "open_url") result = makeOpenUrl(rpcArgs);
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
    const alreadyRan = !!runtime.probeRanRef.current[ackKey] || !!globalOnce[globalKey];
    // Avoid running entity_apply during render; it is handled in an effect so Scene updates reliably.
    // Also allow callers (e.g. history views) to disable auto-running client RPCs entirely.
    if (!disableAutoClientRpcs && autoRun && rpcKind !== "entity_apply" && !alreadyRan && canRun) {
      runtime.markSeenWithLimit(runtime.probeRanRef.current, ackKey, runtime.localUiActionLimit);
      globalOnce[globalKey] = true;
      runtime.pendingAutoRunsRef.current[globalKey] = () => {
        void runRpc().catch(() => {});
      };
    }

    return (
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
              runtime.markSeenWithLimit(runtime.probeRanRef.current, ackKey, runtime.localUiActionLimit);
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
      </Card>
    );
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

  return (
    <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
      <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
        {JSON.stringify(action, null, 2)}
      </pre>
    </Card>
  );
}
