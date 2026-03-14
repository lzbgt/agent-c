import React from "react";

import { daemonFetchInit, daemonHeaders, type ApiAuth } from "../../api";
import type { SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { makeSceneConsole, safeString } from "./sceneViewUtils";

export default function DomEntityView({
  entity,
  baseUrl,
  yolo,
  allowAutoplay,
  sessionId,
  daemonAuth,
  onScriptError,
}: {
  entity: SceneEntity;
  baseUrl?: string;
  yolo?: boolean;
  allowAutoplay?: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  onScriptError?: (args: SceneScriptErrorArgs) => void;
}) {
  const rootRef = React.useRef<HTMLDivElement | null>(null);
  const cleanupRef = React.useRef<null | (() => void)>(null);
  const blobUrlsRef = React.useRef<string[]>([]);

  const props = entity.props ?? {};
  const htmlRaw = safeString(props?.html ?? props?.markup ?? props?.inner_html ?? props?.innerHTML);
  const html = htmlRaw.length > 1_000_000 ? htmlRaw.slice(0, 1_000_000) : htmlRaw;

  const scriptRaw = safeString(props?.script ?? props?.js ?? props?.code);
  const script = scriptRaw.length > 1_000_000 ? scriptRaw.slice(0, 1_000_000) : scriptRaw;

  const scriptArgs = props?.script_args ?? props?.args ?? {};
  const scriptArgsJson = React.useMemo(() => {
    try {
      const s = JSON.stringify(scriptArgs ?? {});
      return s.length > 128_000 ? s.slice(0, 128_000) : s;
    } catch {
      return "{}";
    }
  }, [scriptArgs]);

  React.useEffect(() => {
    const root = rootRef.current;
    if (!root) return;

    try {
      cleanupRef.current?.();
    } catch {
      // ignore
    }
    cleanupRef.current = null;
    blobUrlsRef.current.forEach((u) => {
      try {
        URL.revokeObjectURL(u);
      } catch {
        // ignore
      }
    });
    blobUrlsRef.current = [];

    try {
      root.innerHTML = html || "";
    } catch {
      // ignore
    }

    const reportError = (e: any) => {
      const msg = String(e || "unknown error");
      const stack = e && typeof e === "object" && typeof e.stack === "string" ? e.stack : "";
      try {
        onScriptError?.({
          entity_id: entity.id,
          entity_kind: entity.kind,
          error: msg,
          stack_preview: typeof stack === "string" ? stack.slice(0, 1200) : "",
          script_preview: script.slice(0, 800),
        });
      } catch {
        // ignore
      }
    };

    if (!baseUrl || typeof yolo !== "boolean") return;
    if (!script || script.trim().length === 0) return;

    const sid = safeString(sessionId).trim();
    const tokenRaw = safeString((daemonAuth as any)?.token).trim();
    const agentdTokenRaw = safeString((daemonAuth as any)?.agentdToken).trim();
    const daemon = {
      base_url: baseUrl,
      yolo: !!yolo,
      auth_token: tokenRaw.replace(/^bearer\\s+/i, "").trim(),
      agentd_auth_token: agentdTokenRaw.replace(/^bearer\\s+/i, "").trim(),
      headers: daemonHeaders(daemonAuth),
      session_id: sid || undefined,
    };
    const api: any = {
      root,
      daemon,
      artifact: {
        url: async (path: any) => {
          const p = String(path ?? "").trim();
          if (!p) throw new Error("artifact.url requires path");
          const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
          const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(p)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
          const r = await fetch(src, daemonFetchInit(daemonAuth));
          if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
          const b = await r.blob();
          const u = URL.createObjectURL(b);
          blobUrlsRef.current.push(u);
          return u;
        },
      },
    };

    let cancelled = false;
    void (async () => {
      try {
        const trimmed = script.trim();
        const looksLikeFnExpr =
          trimmed.startsWith("function") ||
          trimmed.startsWith("async function") ||
          trimmed.startsWith("(") ||
          trimmed.startsWith("async (") ||
          trimmed.startsWith("async(") ||
          /^[a-zA-Z_$][\\w$]*\\s*=>/.test(trimmed);

        const body = looksLikeFnExpr
          ? `
const __fn = (${script});
if (typeof __fn === "function") return await __fn(api, args);
return __fn;
`
          : script;

        const fn = new Function(
          "api",
          "args",
          "__console",
          '"use strict"; const console = __console; return (async function() {\n' + body + "\n})();",
        ) as (api: any, args: any, __console: any) => Promise<any>;
        const res = await fn(api, scriptArgs, makeSceneConsole());
        if (!cancelled && allowAutoplay) {
          const g: any = typeof globalThis !== "undefined" ? (globalThis as any) : {};
          const unlocked = !!g.__agentui_autoplay_unlocked;
          if (unlocked && root) {
            const els = Array.from(root.querySelectorAll("audio,video")) as HTMLMediaElement[];
            for (const el of els) {
              try {
                if (el.paused) void el.play();
              } catch {
                // ignore
              }
            }
          }
        }
        if (cancelled) return;
        if (typeof res === "function") cleanupRef.current = res as any;
        else if (res && typeof res === "object" && typeof (res as any).cleanup === "function") cleanupRef.current = (res as any).cleanup;
      } catch (e) {
        if (!cancelled) reportError(e);
      }
    })();

    return () => {
      cancelled = true;
      try {
        cleanupRef.current?.();
      } catch {
        // ignore
      }
      cleanupRef.current = null;
      blobUrlsRef.current.forEach((u) => {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      });
      blobUrlsRef.current = [];
    };
  }, [allowAutoplay, baseUrl, daemonAuth, entity.id, entity.kind, html, onScriptError, script, scriptArgs, scriptArgsJson, sessionId, yolo]);

  return (
    <div className="mt-2 overflow-auto rounded-md border border-black/10 bg-white p-2 text-slate-900">
      <div ref={rootRef} />
    </div>
  );
}
