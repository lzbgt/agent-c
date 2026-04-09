import React from "react";

import { type ApiAuth } from "../../api";
import type { SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import {
  createSceneArtifactUrlResolver,
  getSceneCleanup,
  getSceneDaemonContext,
  isSceneAutoplayUnlocked,
  makeSceneConsole,
  safeObject,
  safeString,
  safeToString,
  type SceneDaemonContext,
  type UnknownRecord,
} from "./sceneViewUtils";

type SceneDomScriptApi = {
  root: HTMLDivElement;
  daemon: SceneDaemonContext;
  artifact: {
    url: (path: unknown) => Promise<string>;
  };
};

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

  const props = safeObject(entity.props);
  const htmlRaw = safeString(props.html ?? props.markup ?? props.inner_html ?? props.innerHTML);
  const html = htmlRaw.length > 1_000_000 ? htmlRaw.slice(0, 1_000_000) : htmlRaw;

  const scriptRaw = safeString(props.script ?? props.js ?? props.code);
  const script = scriptRaw.length > 1_000_000 ? scriptRaw.slice(0, 1_000_000) : scriptRaw;

  const scriptArgs = safeObject(props.script_args ?? props.args);
  const scriptArgsJson = React.useMemo(() => {
    try {
      const s = JSON.stringify(scriptArgs);
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

    const reportError = (error: unknown) => {
      const errorRecord = safeObject(error);
      const msg = safeToString(error) || "unknown error";
      const stack = safeString(errorRecord.stack);
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

    const api: SceneDomScriptApi = {
      root,
      daemon: getSceneDaemonContext({ baseUrl, yolo: !!yolo, sessionId, daemonAuth }),
      artifact: {
        url: createSceneArtifactUrlResolver({ baseUrl, yolo: !!yolo, sessionId, daemonAuth, blobUrlsRef }),
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
        ) as (api: SceneDomScriptApi, args: UnknownRecord, __console: Console) => Promise<unknown>;
        const res = await fn(api, scriptArgs, makeSceneConsole());
        if (!cancelled && allowAutoplay && isSceneAutoplayUnlocked() && root) {
            const els = Array.from(root.querySelectorAll("audio,video")) as HTMLMediaElement[];
            for (const el of els) {
              try {
                if (el.paused) void el.play();
              } catch {
                // ignore
              }
            }
        }
        if (cancelled) return;
        cleanupRef.current = getSceneCleanup(res);
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
