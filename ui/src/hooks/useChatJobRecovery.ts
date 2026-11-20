import React from "react";
import type { ApiAuth } from "../api/auth";
import { ChatJobLookupError, lookupChatJob } from "../api/chatJobLookup";
import { parseChatSubmissionReceipt } from "./useChatSubmissionFence";

type Found = Awaited<ReturnType<typeof lookupChatJob>>;
type Args = {
  base: string; scope: string; sessionId: string; auth: ApiAuth;
  pending: string | null; submitting: boolean;
  onRecovered: (found: Found, traceId: string) => void;
  onFailure: (message: string) => void;
};

function pause(delay: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const abort = () => { clearTimeout(timer); reject(new Error("Recovery cancelled")); };
    const timer = setTimeout(() => { signal.removeEventListener("abort", abort); resolve(); }, delay);
    if (signal.aborted) abort();
    else signal.addEventListener("abort", abort, { once: true });
  });
}

// Reconciliation may repeat reads. It must never repeat a submission.
export default function useChatJobRecovery(args: Args) {
  const context = JSON.stringify([args.scope, args.base, args.sessionId, args.auth]);
  const key = JSON.stringify([context, args.pending]);
  const latest = React.useRef(args);
  latest.current = args;
  const currentKey = React.useRef(key);
  currentKey.current = key;
  const automaticKey = React.useRef<string | null>(null);
  const inFlight = React.useRef<AbortController | null>(null);
  const [checkingKey, setCheckingKey] = React.useState<string | null>(null);

  const check = React.useCallback(async (controller: AbortController, automatic: boolean) => {
    const original = latest.current;
    if (!original.pending || original.submitting) return;
    const receipt = parseChatSubmissionReceipt(original.pending);
    if (!receipt || receipt.session_id !== (original.sessionId || "default") || receipt.mode !== "async") {
      if (!automatic) original.onFailure("No matching asynchronous submission receipt is available. The request remains held.");
      return;
    }
    inFlight.current?.abort();
    inFlight.current = controller;
    setCheckingKey(key);
    try {
      const attempts = automatic ? 3 : 1;
      let found: Found | undefined;
      for (let attempt = 0; attempt < attempts; attempt++) {
        if (attempt) await pause(attempt === 1 ? 250 : 1000, controller.signal);
        try {
          found = await lookupChatJob(original.base, receipt.trace_id, receipt.session_id, original.auth, controller.signal);
          break;
        } catch (error) {
          if (controller.signal.aborted || currentKey.current !== key) return;
          // Identity/schema errors, auth rejection and ambiguous matches are
          // not timing failures. Retain the fence instead of guessing.
          if (error instanceof ChatJobLookupError && ![404,408,429,500,502,503,504].includes(error.status)) throw error;
          if (attempt === attempts - 1) throw error;
        }
      }
      if (found && !controller.signal.aborted && currentKey.current === key) {
        latest.current.onRecovered(found, receipt.trace_id);
      }
    } catch (error) {
      if (!controller.signal.aborted && currentKey.current === key) {
        latest.current.onFailure(`${error instanceof Error ? error.message : "Receipt lookup failed"}. The original request remains held.`);
      }
    } finally {
      if (inFlight.current === controller) {
        inFlight.current = null;
        setCheckingKey(null);
      }
    }
  }, [key]);

  React.useEffect(() => {
    if (!args.pending || args.submitting || automaticKey.current === key) return;
    automaticKey.current = key;
    const controller = new AbortController();
    let settled = false;
    void check(controller, true).finally(() => { settled = true; });
    return () => {
      controller.abort();
      // React StrictMode restarts effects; an aborted probe is not a completed
      // automatic reconciliation cycle.
      if (!settled && automaticKey.current === key) automaticKey.current = null;
    };
  }, [key, args.pending, args.submitting, check]);
  React.useEffect(() => () => inFlight.current?.abort(), []);

  return {
    checking: checkingKey === key,
    recover: async () => {
      if (args.submitting || (checkingKey === key && inFlight.current)) return;
      await check(new AbortController(), false);
    },
  };
}
