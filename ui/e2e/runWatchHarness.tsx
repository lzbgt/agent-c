import React from "react";
import { createRoot } from "react-dom/client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useRunWatchState } from "../src/hooks/useRunWatchState";
import useRunExecution from "../src/hooks/useRunExecution";
import useJobStreaming from "../src/hooks/useJobStreaming";

const noop = () => {};
export function mount(options: { strict?: boolean; queue?: boolean; broker?: boolean } = {}) {
  const node = document.createElement("div");document.body.append(node);
  function Harness() {
    const [scope, setScope] = React.useState("scope-a");
    const [prefs, setPrefs] = React.useState({ base: location.origin, client: "watch-fixture", token: "fixture", broker: options.broker });
    const auth = React.useMemo(() => ({ mode: prefs.broker ? "broker" as const : "direct" as const, token: prefs.token }), [prefs]);
    const [active, setActive] = React.useState<string | null>(null);
    const [status, setStatus] = React.useState<string | null>(null);
    const [result, setResult] = React.useState<any>();
    const [notice, setNotice] = React.useState<string | null>(null);
    const [queue, setQueue] = React.useState(options.queue ? [
      { prompt: "queued first", attachments: [], queued_unix_ms: 1 },
      { prompt: "queued second", attachments: [], queued_unix_ms: 2 },
    ] : []);
    const cursor = React.useRef(0), lastPrompt = React.useRef("");
    const common = { activeJobId: active, daemonAuth: auth, effectiveBase: location.origin,
      jobStoreKey: scope, sessionId: scope, cursorRef: cursor, lastRunPromptRef: lastPrompt,
      setActiveJobId: setActive, setJobStatus: setStatus, setJobError: noop, setJobUpdatedMs: noop,
      setLiveEvents: noop, setResult, setJobNotice: setNotice, setLastCompletedPrompt: noop,
      auditRefetch: noop, sessionsRefetch: noop };
    const watch = useRunWatchState({ ...common, authKey: prefs.token, runWatchCanUse: true,
      runWatchPrefsBase: prefs.base, runWatchPrefsClientId: prefs.client });
    const run = useRunExecution({ ...common, effectiveUseAsync: true, client: {id:"fixture",kind:"fixture",instance_id:"fixture"},
      tools: "none", runQueue: queue, setRunQueue: setQueue, setPrompt: noop, setComposerTaskNonce: noop,
      setLastRunPrompt: noop, writeJobsBySession: watch.writeJobsBySession } as any);
    useJobStreaming({ ...common, connectionMode: "direct", daemonAuthToken: prefs.token,
      effectiveSseBase: location.origin, writeJobsBySession: watch.writeJobsBySession });
    (window as any).runWatchTest = { active, status, result, notice, queue, mode: watch.runWatchMode,
      pending: run.pendingChatSubmission,
      send: () => run.handleDirectRunRequest({ prompt: "hello", attachments: [] }),
      setScope, changePrefs: (next: Partial<typeof prefs>) => setPrefs(value => ({ ...value, ...next })),
      touch: () => watch.writeJobsBySession(previous => ({ ...previous, [scope]: { ...previous[scope], cursor: ++cursor.current } })),
    };
    return React.createElement("div", null, notice);
  }
  const content = React.createElement(QueryClientProvider, {client: new QueryClient()}, React.createElement(Harness));
  createRoot(node).render(options.strict ? React.createElement(React.StrictMode, null, content) : content);
}
