import React from "react";
import { createRoot } from "react-dom/client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import useRunExecution from "../src/hooks/useRunExecution";
export function mount() {
    const node = document.createElement("div"); document.body.append(node);
    function Harness() {
      const [queue, setQueue] = React.useState([
        { prompt: "first request", attachments: [], queued_unix_ms: 1 },
        { prompt: "second request", attachments: [], queued_unix_ms: 2 },
      ]);
      const [prompt, setPrompt] = React.useState("");
      const noop = () => {};
      const value = useRunExecution({
        activeJobId: null, effectiveBase: location.origin, daemonAuth: {},
        effectiveUseAsync: true, jobStoreKey: "fence-test-scope", sessionId: "fence-test",
        client: { id: "test", kind: "test", instance_id: "test" }, tools: "none",
        runQueue: queue, setRunQueue: setQueue, setPrompt,
        cursorRef: React.useRef(0), lastRunPromptRef: React.useRef(""),
        setActiveJobId: noop, setComposerTaskNonce: noop, setJobError: noop, setJobNotice: noop,
        setJobStatus: noop, setJobUpdatedMs: noop, setLastCompletedPrompt: noop,
        setLastRunPrompt: noop, setLiveEvents: noop, setResult: noop,
        writeJobsBySession: noop, auditRefetch: noop, sessionsRefetch: noop,
      } as any);
      (window as any).chatFenceTest = { queue, prompt, pending: value.pendingChatSubmission, pendingMutation: value.run.isPending };
      return React.createElement("div", null, prompt);
    }
    createRoot(node).render(React.createElement(QueryClientProvider, { client: new QueryClient() }, React.createElement(Harness)));
}
