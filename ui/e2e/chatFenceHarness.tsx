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
      const [active, setActive] = React.useState<string | null>(null);
      const noop = () => {};
      const value = useRunExecution({
        activeJobId: active, effectiveBase: location.origin, daemonAuth: {},
        effectiveUseAsync: true, jobStoreKey: "fence-test-scope", sessionId: "fence-test",
        client: { id: "test", kind: "test", instance_id: "test" }, tools: "none",
        runQueue: queue, setRunQueue: setQueue, setPrompt,
        cursorRef: React.useRef(0), lastRunPromptRef: React.useRef(""),
        setActiveJobId: setActive, setComposerTaskNonce: noop, setJobError: noop, setJobNotice: noop,
        setJobStatus: noop, setJobUpdatedMs: noop, setLastCompletedPrompt: noop,
        setLastRunPrompt: noop, setLiveEvents: noop, setResult: noop,
        writeJobsBySession: noop, auditRefetch: noop, sessionsRefetch: noop,
      } as any);
      (window as any).chatFenceTest = { queue, prompt, active, recover: value.recoverChatSubmission, pending: value.pendingChatSubmission, pendingMutation: value.run.isPending };
      return React.createElement("div", null, prompt);
    }
    createRoot(node).render(React.createElement(QueryClientProvider, { client: new QueryClient() }, React.createElement(Harness)));
}
