import React from "react";
const prefix = "agentui.pendingChatSubmission.";
export function readChatFence(scope: string): string | null {
  try { return localStorage.getItem(prefix + scope); }
  catch { return "Browser storage unavailable; no chat can be dispatched safely."; }
}
export function beginChatFence(scope: string, traceId: string) {
  if (readChatFence(scope)) throw new Error("Previous chat submission unresolved; inspect its outcome before continuing.");
  const value = JSON.stringify({ trace_id: traceId, created_unix_ms: Date.now() });
  localStorage.setItem(prefix + scope, value);
  if (localStorage.getItem(prefix + scope) !== value) throw new Error("Unable to persist chat submission receipt");
}
export function finishChatFence(scope: string, traceId: string) {
  const raw = localStorage.getItem(prefix + scope);
  if (raw && JSON.parse(raw).trace_id === traceId) localStorage.removeItem(prefix + scope);
}
export default function useChatSubmissionFence(scope: string) {
  const blocked = React.useRef<{ scope: string; error: string } | null>(null);
  const [, refresh] = React.useReducer(n => n + 1, 0);
  const currentScope = React.useRef(scope);
  currentScope.current = scope;
  React.useEffect(() => {
    const listener = () => refresh();
    window.addEventListener("storage", listener);
    return () => window.removeEventListener("storage", listener);
  }, []);
  return {
    pending: readChatFence(scope) || (blocked.current?.scope === scope ? blocked.current.error : null), currentScope,
    isBlocked: () => Boolean(readChatFence(scope) || blocked.current?.scope === scope),
    begin: (trace: string) => {
      try { beginChatFence(scope, trace); refresh(); }
      catch (e) { blocked.current = { scope, error: String(e) }; refresh(); throw e; }
    },
    finish: (origin: string, trace: string) => { finishChatFence(origin, trace); refresh(); },
    release: () => {
      if (!window.confirm("The original chat may have executed. Check its trace/job outcome first. Clear this local fence and allow queued/new messages? This does not retry or cancel the original request.")) return;
      try { localStorage.removeItem(prefix + scope); blocked.current = null; refresh(); }
      catch { /* remain held */ }
    },
  };
}
