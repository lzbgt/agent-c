import { safeObject } from "../jsonUtils";
import { daemonFetchInit, type ApiAuth } from "./auth";
import {
  JobCancelRespSchema,
  type JobCancelResp,
  JobRespSchema,
  type JobResp,
  RunAttestationRespSchema,
  type RunAttestationResp,
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
  const cryptoApi =
    typeof globalThis === "object" && "crypto" in globalThis ? globalThis.crypto : undefined;
  if (cryptoApi && typeof cryptoApi.randomUUID === "function") {
    return `trace_${cryptoApi.randomUUID()}`;
  }
  return `trace_${Date.now()}_${Math.random().toString(16).slice(2)}_${Math.random().toString(16).slice(2)}`;
}

async function parseJsonObject(response: Response): Promise<Record<string, unknown>> {
  try {
    return safeObject(await response.json());
  } catch {
    return {};
  }
}

function ensureTraceId(req: RunRequest): RunRequest {
  if (typeof req.trace_id === "string" && req.trace_id.length > 0) return req;
  return { ...req, trace_id: newTraceId() };
}

export async function apiRun(base: string, req: RunRequest, auth?: ApiAuth): Promise<RunResponse> {
  const payload = ensureTraceId(RunRequestSchema.parse(req));
  const r = await fetch(
    `${base}/api/v1/run`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return RunResponseSchema.parse(j);
}

export async function apiRunAsync(base: string, req: RunRequest, auth?: ApiAuth): Promise<RunAsyncResp> {
  const payload = ensureTraceId(RunRequestSchema.parse(req));
  const r = await fetch(
    `${base}/api/v1/run_async`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return RunAsyncRespSchema.parse(j);
}

export async function apiGetJob(base: string, jobId: string, auth?: ApiAuth): Promise<JobResp> {
  const r = await fetch(`${base}/api/v1/job?job_id=${encodeURIComponent(jobId)}`, daemonFetchInit(auth));
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
    daemonFetchInit(auth),
  );
  const j = await r.json();
  return JobRespSchema.parse(j);
}

export async function apiCancelJob(base: string, jobId: string, auth?: ApiAuth): Promise<JobCancelResp> {
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 10_000);
  try {
    const r = await fetch(`${base}/api/v1/job/cancel?job_id=${encodeURIComponent(jobId)}`,
      daemonFetchInit(auth, { method: "POST", signal: controller.signal }));
    const reply = JobCancelRespSchema.parse({ ...await parseJsonObject(r), status: r.status });
    if (r.ok && reply.ok && reply.job_id === jobId) return reply;
    const authRejected = reply.job_id === undefined && (r.status === 401 || r.status === 403);
    if (!reply.ok && r.status >= 400 && r.status < 500 && (reply.job_id === jobId || authRejected)) return reply;
    // A missing/wrong-job receipt or a gateway/server failure cannot say
    // whether native cancellation was requested. Never synthesize an outcome.
    throw new Error("No matching Stop acknowledgement");
  } finally {
    clearTimeout(deadline);
  }
}

export async function apiRunReplay(base: string, runId: string, auth?: ApiAuth): Promise<RunReplayResp> {
  const rid = String(runId || "").trim();
  if (!rid) throw new Error("missing run_id");
  const r = await fetch(`${base}/api/v1/run/replay?run_id=${encodeURIComponent(rid)}`, daemonFetchInit(auth));
  const j = await parseJsonObject(r);
  if (!r.ok) {
    return RunReplayRespSchema.parse({ ok: false, ...j });
  }
  return RunReplayRespSchema.parse(j);
}

export async function apiRunAttestation(base: string, runId: string, auth?: ApiAuth): Promise<RunAttestationResp> {
  const rid = String(runId || "").trim();
  if (!rid) throw new Error("missing run_id");
  const r = await fetch(`${base}/api/v1/run/attestation?run_id=${encodeURIComponent(rid)}`, daemonFetchInit(auth));
  const j = await parseJsonObject(r);
  if (!r.ok) {
    return RunAttestationRespSchema.parse({ ok: false, ...j });
  }
  return RunAttestationRespSchema.parse(j);
}
