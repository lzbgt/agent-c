import { daemonHeaders, type ApiAuth } from "./auth";
import {
  JobRespSchema,
  type JobResp,
  RunAsyncRespSchema,
  type RunAsyncResp,
  RunReplayRespSchema,
  type RunReplayResp,
  RunRequestSchema,
  type RunRequest,
  RunResponseSchema,
  type RunResponse,
} from "./schemas/run";

function newTraceId(): string {
  // Must match agentd's safe trace_id character set: [A-Za-z0-9_.:@-]
  // Prefer a stable, low-collision value when available.
  const anyCrypto: any = (globalThis as any).crypto;
  if (anyCrypto && typeof anyCrypto.randomUUID === "function") {
    return `trace_${anyCrypto.randomUUID()}`;
  }
  return `trace_${Date.now()}_${Math.random().toString(16).slice(2)}_${Math.random().toString(16).slice(2)}`;
}

function ensureTraceId(req: RunRequest): RunRequest {
  if (typeof req.trace_id === "string" && req.trace_id.length > 0) return req;
  return { ...req, trace_id: newTraceId() };
}

export async function apiRun(base: string, req: RunRequest, auth?: ApiAuth): Promise<RunResponse> {
  const payload = ensureTraceId(RunRequestSchema.parse(req));
  const r = await fetch(`${base}/api/v1/run`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return RunResponseSchema.parse(j);
}

export async function apiRunAsync(base: string, req: RunRequest, auth?: ApiAuth): Promise<RunAsyncResp> {
  const payload = ensureTraceId(RunRequestSchema.parse(req));
  const r = await fetch(`${base}/api/v1/run_async`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return RunAsyncRespSchema.parse(j);
}

export async function apiGetJob(base: string, jobId: string, auth?: ApiAuth): Promise<JobResp> {
  const r = await fetch(`${base}/api/v1/job?job_id=${encodeURIComponent(jobId)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return JobRespSchema.parse(j);
}

export async function apiGetJobProgress(
  base: string,
  jobId: string,
  auth?: ApiAuth,
  opts?: { cursor?: number; maxEvents?: number },
): Promise<JobResp> {
  const cursor = opts?.cursor ?? 0;
  const maxEvents = opts?.maxEvents ?? 256;
  const r = await fetch(
    `${base}/api/v1/job?job_id=${encodeURIComponent(jobId)}&include_events=1&cursor=${encodeURIComponent(
      String(cursor),
    )}&max_events=${encodeURIComponent(String(maxEvents))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return JobRespSchema.parse(j);
}

export async function apiCancelJob(base: string, jobId: string, auth?: ApiAuth): Promise<any> {
  const r = await fetch(`${base}/api/v1/job/cancel?job_id=${encodeURIComponent(jobId)}`, {
    method: "POST",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return j;
}

export async function apiRunReplay(base: string, runId: string, auth?: ApiAuth): Promise<RunReplayResp> {
  const rid = String(runId || "").trim();
  if (!rid) throw new Error("missing run_id");
  const r = await fetch(`${base}/api/v1/run/replay?run_id=${encodeURIComponent(rid)}`, {
    headers: daemonHeaders(auth),
  });
  let j: any = {};
  try {
    j = await r.json();
  } catch {
    j = {};
  }
  if (!r.ok) {
    return RunReplayRespSchema.parse({ ok: false, ...j });
  }
  return RunReplayRespSchema.parse(j);
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function apiRunMaybeAsync(
  base: string,
  req: RunRequest,
  auth?: ApiAuth,
  opts?: { pollMs?: number; timeoutMs?: number },
): Promise<RunResponse> {
  req = ensureTraceId(req);
  const pollMs = opts?.pollMs ?? 500;
  const timeoutMs = opts?.timeoutMs ?? 120_000;
  const started = Date.now();

  // Prefer async when available; fall back to sync if endpoint missing.
  let asyncResp: RunAsyncResp | undefined;
  try {
    asyncResp = await apiRunAsync(base, req, auth);
  } catch {
    // likely 404 or JSON mismatch; fall back to sync.
    return apiRun(base, req, auth);
  }

  if (!asyncResp.ok || !asyncResp.job_id) {
    // If daemon reported an error, fall back to sync as best-effort.
    return apiRun(base, req, auth);
  }

  while (true) {
    if (Date.now() - started > timeoutMs) {
      throw new Error(`Timed out waiting for job ${asyncResp.job_id}`);
    }
    const job = await apiGetJob(base, asyncResp.job_id, auth);
    if (!job.ok) {
      throw new Error(job.error || "job failed");
    }
    if (job.status === "done" || job.status === "error") {
      if (!job.result) {
        throw new Error("job completed but missing result");
      }
      return job.result;
    }
    await sleep(pollMs);
  }
}
