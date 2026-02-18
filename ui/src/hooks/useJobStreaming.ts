import React from "react";
import { apiGetJob, apiGetJobProgress, daemonHeaders, type AgentEvent, type ApiAuth, type RunResponse } from "../api";
import { appendLiveEvents, capLiveEvents } from "../liveEvents";
import { readSseStream } from "../sse";
import { sleep } from "../timeUtils";

type JobStoreWriter = (mutate: (prev: Record<string, any>) => Record<string, any>) => void;

export type JobStreamingArgs = {
  activeJobId: string | null;
  connectionMode: string;
  daemonAuth?: ApiAuth;
  daemonAuthToken?: string | null;
  effectiveBase: string;
  effectiveSseBase: string;
  sessionId: string | null;
  jobStoreKey: string;
  cursorRef: React.MutableRefObject<number>;
  lastRunPromptRef: React.MutableRefObject<string>;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobNotice: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
  setResult: React.Dispatch<React.SetStateAction<RunResponse | undefined>>;
  setLastCompletedPrompt: React.Dispatch<React.SetStateAction<string>>;
  writeJobsBySession: JobStoreWriter;
  auditRefetch: () => unknown;
  sessionsRefetch: () => unknown;
};

export default function useJobStreaming(args: JobStreamingArgs) {
  const {
    activeJobId,
    connectionMode,
    daemonAuth,
    daemonAuthToken,
    effectiveBase,
    effectiveSseBase,
    sessionId,
    jobStoreKey,
    cursorRef,
    lastRunPromptRef,
    setActiveJobId,
    setJobStatus,
    setJobError,
    setJobNotice,
    setJobUpdatedMs,
    setLiveEvents,
    setResult,
    setLastCompletedPrompt,
    writeJobsBySession,
    auditRefetch,
    sessionsRefetch,
  } = args;

  React.useEffect(() => {
    if (!activeJobId) return;
    let cancelled = false;
    const jobId = activeJobId;
    let watchdogTimer: any = null;

    const startPolling = () => {
      (async () => {
        // Poll job progress + stream events via cursor.
        for (;;) {
          if (cancelled) return;
          let job: any;
          try {
            job = await apiGetJobProgress(effectiveBase, jobId, daemonAuth, { cursor: cursorRef.current, maxEvents: 256 });
          } catch (e) {
            // Transient fetch failures should not invalidate the visible conversation.
            // Keep the current liveEvents and keep the job active; retry with backoff.
            setJobNotice(`job fetch failed (retrying): ${String(e)}`);
            await sleep(1000);
            continue;
          }

          if (cancelled) return;
          setJobStatus(job.status ?? null);
          setJobError(job.status === "error" || job.status === "interrupted" ? (job.error ?? null) : null);
          setJobNotice(null);
          setJobUpdatedMs(typeof job.updated_unix_ms === "number" ? job.updated_unix_ms : null);

          const ev = Array.isArray(job.events) ? job.events : [];
          const next = typeof job.events_cursor_next === "number" ? job.events_cursor_next : cursorRef.current + ev.length;
          if (ev.length > 0) {
            if (job.events_reset) {
              setLiveEvents(capLiveEvents(ev));
            } else {
              setLiveEvents((prev) => appendLiveEvents(prev, ev));
            }
            cursorRef.current = next;
          }

          if (job.status === "done" || job.status === "error" || job.status === "cancelled" || job.status === "interrupted") {
            if (job.result) {
              setResult(job.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            } else {
              setJobError("job completed but missing result");
            }
            setJobNotice(null);
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[jobStoreKey];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
            return;
          }

          await sleep(500);
        }
      })().catch((e) => {
        if (cancelled) return;
        // Keep job state visible; do not invalidate the conversation on unexpected polling loop errors.
        setJobNotice(`polling loop failed (retrying): ${String(e)}`);
      });
    };

    // Prefer SSE streaming when available. Fall back to polling.
    let es: EventSource | null = null;
    let fetchAbort: AbortController | null = null;
    let fetchFinished = false;
    const canUseEventSource =
      typeof EventSource !== "undefined" &&
      connectionMode === "direct" &&
      (!daemonAuthToken || daemonAuthToken.trim().length === 0) &&
      typeof effectiveSseBase === "string" &&
      (effectiveSseBase.startsWith("http://") || effectiveSseBase.startsWith("https://"));
    const canUseFetchSse =
      typeof fetch !== "undefined" &&
      typeof effectiveSseBase === "string" &&
      (effectiveSseBase.startsWith("http://") || effectiveSseBase.startsWith("https://"));

    let fallbackStarted = false;
    const fallbackToPolling = () => {
      if (fallbackStarted) return;
      fallbackStarted = true;
      startPolling();
    };

    // Watchdog: even if SSE/streaming is flaky, ensure we eventually observe terminal state.
    // This avoids "stuck Running…" UIs when the stream drops before a job_done event.
    watchdogTimer = setInterval(() => {
      if (cancelled) return;
      void (async () => {
        try {
          const job = await apiGetJob(effectiveBase, jobId, daemonAuth);
          if (!job?.ok) return;
          if (job.status === "done" || job.status === "error" || job.status === "cancelled" || job.status === "interrupted") {
            // Trigger the same completion path as streaming.
            if (job.result) {
              setResult(job.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
              setJobStatus(job.status);
              setJobError(job.status === "error" || job.status === "interrupted" ? (job.error ?? null) : null);
              setJobNotice(null);
            } else {
              setJobError("job completed but missing result");
              setJobNotice(null);
            }
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[jobStoreKey];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
          }
        } catch {
          // ignore; watchdog is best-effort
        }
      })();
    }, 3000);

    const startFetchSse = () => {
      const url = `${effectiveSseBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
        String(cursorRef.current),
      )}`;
      const controller = new AbortController();
      fetchAbort = controller;
      setJobStatus("running");
      setJobUpdatedMs(null);

      (async () => {
        const resp = await fetch(url, {
          headers: daemonHeaders(daemonAuth),
          signal: controller.signal,
        });
        if (!resp.ok) {
          throw new Error(`SSE fetch failed: HTTP ${resp.status}`);
        }
        await readSseStream(resp, (evt) => {
          if (cancelled) return;
          if (evt.id && evt.id.length > 0) {
            const n = Number(evt.id);
            if (Number.isFinite(n)) cursorRef.current = n + 1;
          }
          if (evt.event === "reset") {
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              if (typeof data?.cursor_base === "number") {
                cursorRef.current = data.cursor_base;
              }
            } catch {
              // ignore
            }
            setLiveEvents([]);
          } else if (evt.event === "agent_event") {
            try {
              const ev = JSON.parse(String(evt.data || "{}"));
              setLiveEvents((prev) => appendLiveEvents(prev, ev));
            } catch {
              // ignore malformed
            }
          } else if (evt.event === "job_done") {
            if (cancelled) return;
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              setJobStatus(typeof data?.status === "string" ? data.status : "done");
              setJobError(
                typeof data?.status === "string" && (data.status === "error" || data.status === "interrupted")
                  ? (typeof data?.error === "string" ? data.error : null)
                  : null,
              );
              setJobNotice(null);
              if (data?.result) {
                setResult(data.result);
                setLastCompletedPrompt(lastRunPromptRef.current);
              }
            } catch {
              setJobNotice("failed to parse job_done event (falling back to polling)");
            }
            fetchFinished = true;
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[jobStoreKey];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
          }
        });
        if (!cancelled && !fetchFinished) {
          // Stream ended without a terminal job_done event; fall back to polling so the UI still progresses.
          fallbackToPolling();
        }
      })().catch((e) => {
        if (cancelled || fetchFinished) return;
        setJobNotice(`job stream dropped (retrying via polling): ${String(e)}`);
        fallbackToPolling();
      });
    };

    if (canUseEventSource) {
      try {
        const url = `${effectiveSseBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
          String(cursorRef.current),
        )}`;
        es = new EventSource(url);
        setJobStatus("running");
        setJobUpdatedMs(null);
        es.onopen = () => {
          // ok
        };
        es.addEventListener("reset", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            if (typeof data?.cursor_base === "number") {
              cursorRef.current = data.cursor_base;
            }
          } catch {
            // ignore
          }
          setLiveEvents([]);
        });
        es.addEventListener("agent_event", (evt: any) => {
          if (cancelled) return;
          try {
            const ev = JSON.parse(String(evt.data || "{}"));
            if (evt && typeof evt.lastEventId === "string" && evt.lastEventId.length > 0) {
              const n = Number(evt.lastEventId);
              if (Number.isFinite(n)) cursorRef.current = n + 1;
            }
            setLiveEvents((prev) => appendLiveEvents(prev, ev));
          } catch {
            // ignore malformed
          }
        });
        es.addEventListener("job_done", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            setJobStatus(typeof data?.status === "string" ? data.status : "done");
            setJobError(
              typeof data?.status === "string" && (data.status === "error" || data.status === "interrupted")
                ? (typeof data?.error === "string" ? data.error : null)
                : null,
            );
            setJobNotice(null);
            if (data?.result) {
              setResult(data.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            }
          } catch {
            setJobNotice("failed to parse job_done event (falling back to polling)");
          }
          setActiveJobId(null);
          const sid = String(sessionId || "").trim();
          if (sid) {
            writeJobsBySession((prev) => {
              const nextm = { ...prev };
              delete nextm[jobStoreKey];
              return nextm;
            });
          }
          try {
            es?.close();
          } catch {
            // ignore
          }
          void auditRefetch();
          void sessionsRefetch();
        });
        es.onerror = () => {
          if (cancelled) return;
          try {
            es?.close();
          } catch {
            // ignore
          }
          // Any SSE failure should fall back to polling (cursor-based) so the UI keeps progressing.
          // Do not clear liveEvents; keep the conversation visible.
          setJobNotice("job stream dropped (retrying via polling)");
          fallbackToPolling();
        };
      } catch {
        fallbackToPolling();
      }
    } else if (canUseFetchSse) {
      startFetchSse();
    } else {
      fallbackToPolling();
    }

    return () => {
      cancelled = true;
      try {
        if (watchdogTimer) clearInterval(watchdogTimer);
      } catch {
        // ignore
      }
      try {
        es?.close();
      } catch {
        // ignore
      }
      try {
        fetchAbort?.abort();
      } catch {
        // ignore
      }
    };
  }, [
    activeJobId,
    auditRefetch,
    connectionMode,
    cursorRef,
    daemonAuth,
    daemonAuthToken,
    effectiveBase,
    effectiveSseBase,
    jobStoreKey,
    lastRunPromptRef,
    sessionId,
    sessionsRefetch,
    setActiveJobId,
    setJobError,
    setJobNotice,
    setJobStatus,
    setJobUpdatedMs,
    setLastCompletedPrompt,
    setLiveEvents,
    setResult,
    writeJobsBySession,
  ]);
}
