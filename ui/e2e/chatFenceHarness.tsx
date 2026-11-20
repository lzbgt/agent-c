import React from "react";
import { createRoot } from "react-dom/client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import useRunExecution from "../src/hooks/useRunExecution";
import { readJobWatches, writeJobWatches } from "../src/jobStore";
import type { RunWatchByScope } from "../src/runWatchPrefs";
export function mount(options: { async?: boolean; strict?: boolean; emptyQueue?: boolean } = {}) {
    const node = document.createElement("div"); document.body.append(node);
    function Harness() {
      const [queue, setQueue] = React.useState(options.emptyQueue ? [] : [
        { prompt: "first request", attachments: [], queued_unix_ms: 1 },
        { prompt: "second request", attachments: [], queued_unix_ms: 2 },
      ]);
      const [prompt, setPrompt] = React.useState("");
      const [active, setActive] = React.useState<string | null>(null);
      const [scope, setScope] = React.useState("fence-test-scope");
      const [session, setSession] = React.useState("fence-test");
      const [auth, setAuth] = React.useState({});
      const [notice, setNotice] = React.useState<string | null>(null);
      const noop = () => {};
      const value = useRunExecution({
        activeJobId: active, effectiveBase: location.origin, daemonAuth: auth,
        effectiveUseAsync: options.async !== false, jobStoreKey: scope, sessionId: session,
        client: { id: "test", kind: "test", instance_id: "test" }, tools: "none",
        runQueue: queue, setRunQueue: setQueue, setPrompt,
        cursorRef: React.useRef(0), lastRunPromptRef: React.useRef(""),
        setActiveJobId: setActive, setComposerTaskNonce: noop, setJobError: noop, setJobNotice: setNotice,
        setJobStatus: noop, setJobUpdatedMs: noop, setLastCompletedPrompt: noop,
        setLastRunPrompt: noop, setLiveEvents: noop, setResult: noop,
        writeJobsBySession: (mutate: (previous: RunWatchByScope) => RunWatchByScope) => { writeJobWatches(mutate(readJobWatches())); },
        auditRefetch: noop, sessionsRefetch: noop,
      } as any);
      (window as any).chatFenceTest = { queue, prompt, active, notice,
        checking: value.checkingChatSubmission, recover: value.recoverChatSubmission,
        pending: value.pendingChatSubmission, pendingMutation: value.run.isPending,
        changeContext: (next: { scope?: string; session?: string; auth?: object }) => {
          setQueue([]);setActive(null);
          if (next.scope) setScope(next.scope);
          if (next.session) setSession(next.session);
          if (next.auth) setAuth(next.auth);
        },
      };
      return React.createElement("div", null, prompt);
    }
    const content = React.createElement(QueryClientProvider, { client: new QueryClient() }, React.createElement(Harness));
    createRoot(node).render(options.strict ? React.createElement(React.StrictMode, null, content) : content);
}
