import { z } from "zod";

export function daemonHeaders(authToken?: string, extra?: Record<string, string>): Record<string, string> {
  const h: Record<string, string> = { ...(extra ?? {}) };
  if (authToken && authToken.trim().length > 0) {
    h["Authorization"] = `Bearer ${authToken.trim()}`;
  }
  return h;
}

export const HealthSchema = z.object({
  ok: z.boolean(),
  service: z.string().optional(),
  version: z.string().optional(),
});
export type Health = z.infer<typeof HealthSchema>;

export const RunRequestSchema = z.object({
  prompt: z.string().min(1),
  session_id: z.string().optional(),
  no_session: z.boolean().optional(),
  system: z.string().optional(),
  no_default_system: z.boolean().optional(),
  model: z.string().optional(),
  summary_model: z.string().optional(),
  summary_max_chars: z.number().int().nonnegative().optional(),
  base_url: z.string().optional(),
  api_key: z.string().optional(),
  proxy: z.string().optional(),
  timeout_ms: z.number().int().positive().optional(),
  stream_assistant: z.boolean().optional(),
  max_capture_bytes: z.number().int().nonnegative().optional(),
  tools: z.enum(["host", "basic", "none"]).optional(),
  tools_root: z.string().optional(),
  host_policy: z.enum(["full", "readonly"]).optional(),
  max_steps: z.number().int().nonnegative().optional(),
  max_chars: z.number().int().nonnegative().optional(),
  keep_last: z.number().int().nonnegative().optional(),
  trace: z.boolean().optional(),
  yolo: z.boolean().optional(),
  verbose: z.boolean().optional(),
});
export type RunRequest = z.infer<typeof RunRequestSchema>;

export const EventSchema = z.object({
  type: z.string(),
  data: z.any().optional(),
});
export type AgentEvent = z.infer<typeof EventSchema>;

export const RunResponseSchema = z.object({
  ok: z.boolean(),
  assistant_text: z.string().optional(),
  error: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
  trace_text: z.string().optional(),
  effective_tools_root: z.string().optional(),
  effective_yolo: z.boolean().optional(),
  effective_host_policy: z.enum(["full", "readonly"]).optional(),
  effective_timeout_ms: z.number().optional(),
  effective_stream_assistant: z.boolean().optional(),
  verbose: z.boolean().optional(),
  events: z.array(EventSchema).optional(),
});
export type RunResponse = z.infer<typeof RunResponseSchema>;

export const ToolDefsRespSchema = z.object({
  ok: z.boolean(),
  tools: z.string().optional(),
  effective_tools_root: z.string().optional(),
  effective_yolo: z.boolean().optional(),
  effective_host_policy: z.enum(["full", "readonly"]).optional(),
  count: z.number().optional(),
  defs: z
    .array(
      z.object({
        name: z.string(),
        description: z.string().optional(),
        parameters_json: z.string().optional(),
      }),
    )
    .optional(),
  error: z.string().optional(),
});
export type ToolDefsResp = z.infer<typeof ToolDefsRespSchema>;

export async function apiGetTools(
  base: string,
  authToken?: string,
  opts?: { tools?: "host" | "basic" | "none"; toolsRoot?: string; yolo?: boolean; hostPolicy?: "full" | "readonly" },
): Promise<ToolDefsResp> {
  const q = new URLSearchParams();
  if (opts?.tools) q.set("tools", opts.tools);
  if (typeof opts?.toolsRoot === "string") q.set("tools_root", opts.toolsRoot);
  if (typeof opts?.yolo === "boolean") q.set("yolo", opts.yolo ? "1" : "0");
  if (opts?.hostPolicy) q.set("host_policy", opts.hostPolicy);
  const r = await fetch(`${base}/api/v1/tools?${q.toString()}`, { headers: daemonHeaders(authToken) });
  const j = await r.json();
  return ToolDefsRespSchema.parse(j);
}

export async function apiCancelJob(base: string, jobId: string, authToken?: string): Promise<any> {
  const r = await fetch(`${base}/api/v1/job/cancel?job_id=${encodeURIComponent(jobId)}`, {
    method: "POST",
    headers: daemonHeaders(authToken),
  });
  const j = await r.json();
  return j;
}

export const OpenRouterModelsRespSchema = z.object({
  ok: z.boolean(),
  source: z.string().optional(),
  base_url: z.string().optional(),
  models_url: z.string().optional(),
  cached: z.boolean().optional(),
  fetched_unix_ms: z.number().optional(),
  min_total: z.number().optional(),
  max_total: z.number().optional(),
  require_multimodal_input: z.boolean().optional(),
  require_tools: z.boolean().optional(),
  include_free: z.boolean().optional(),
  limit: z.number().optional(),
  total_models: z.number().optional(),
  count: z.number().optional(),
  recommended_model: z.string().optional(),
  models: z
    .array(
      z.object({
        id: z.string(),
        name: z.string().optional(),
        context_length: z.any().optional(),
        total_usd_per_million: z.number().optional(),
        prompt_usd_per_million: z.number().optional(),
        completion_usd_per_million: z.number().optional(),
        supports_tools: z.boolean().optional(),
        supports_multimodal_input: z.boolean().optional(),
        input_modalities: z.any().optional(),
        output_modalities: z.any().optional(),
      }),
    )
    .optional(),
  error: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
});
export type OpenRouterModelsResp = z.infer<typeof OpenRouterModelsRespSchema>;

export async function apiGetOpenRouterModels(
  base: string,
  opts: {
    daemonAuthToken?: string;
    apiKey?: string;
    openrouterBaseUrl?: string;
    minTotal?: number;
    maxTotal?: number;
    requireMultimodalInput?: boolean;
    requireTools?: boolean;
    includeFree?: boolean;
    limit?: number;
    refresh?: boolean;
  },
): Promise<OpenRouterModelsResp> {
  const q = new URLSearchParams();
  if (opts.openrouterBaseUrl) q.set("base_url", opts.openrouterBaseUrl);
  if (typeof opts.minTotal === "number") q.set("min_total", String(opts.minTotal));
  if (typeof opts.maxTotal === "number") q.set("max_total", String(opts.maxTotal));
  if (typeof opts.requireMultimodalInput === "boolean")
    q.set("require_multimodal_input", opts.requireMultimodalInput ? "1" : "0");
  if (typeof opts.requireTools === "boolean") q.set("require_tools", opts.requireTools ? "1" : "0");
  if (typeof opts.includeFree === "boolean") q.set("include_free", opts.includeFree ? "1" : "0");
  if (typeof opts.limit === "number") q.set("limit", String(opts.limit));
  if (opts.refresh) q.set("refresh", "1");

  const headers: Record<string, string> = daemonHeaders(opts.daemonAuthToken);
  if (opts.apiKey && opts.apiKey.trim().length > 0) {
    headers["X-OpenRouter-Key"] = opts.apiKey.trim();
  }
  const r = await fetch(`${base}/api/v1/openrouter/models?${q.toString()}`, { headers });
  const j = await r.json();
  return OpenRouterModelsRespSchema.parse(j);
}

export async function apiGetHealth(base: string, authToken?: string): Promise<Health> {
  const r = await fetch(`${base}/api/v1/health`, { headers: daemonHeaders(authToken) });
  const j = await r.json();
  return HealthSchema.parse(j);
}

export const SessionsSchema = z.object({
  ok: z.boolean(),
  sessions: z.array(z.string()).optional(),
  error: z.string().optional(),
});
export type SessionsResp = z.infer<typeof SessionsSchema>;

export async function apiListSessions(base: string, authToken?: string): Promise<SessionsResp> {
  const r = await fetch(`${base}/api/v1/sessions`, { headers: daemonHeaders(authToken) });
  const j = await r.json();
  return SessionsSchema.parse(j);
}

export const SessionSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  messages: z.array(z.object({ role: z.string(), content: z.string() })).optional(),
  error: z.string().optional(),
});
export type SessionResp = z.infer<typeof SessionSchema>;

export async function apiGetSession(base: string, sessionId: string, authToken?: string): Promise<SessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`, { headers: daemonHeaders(authToken) });
  const j = await r.json();
  return SessionSchema.parse(j);
}

export const AuditSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  entries: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type AuditResp = z.infer<typeof AuditSchema>;

export async function apiGetAudit(base: string, sessionId: string, authToken?: string): Promise<AuditResp> {
  const r = await fetch(`${base}/api/v1/session/audit?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`, {
    headers: daemonHeaders(authToken),
  });
  const j = await r.json();
  return AuditSchema.parse(j);
}

export async function apiRun(base: string, req: RunRequest, authToken?: string): Promise<RunResponse> {
  const payload = RunRequestSchema.parse(req);
  const r = await fetch(`${base}/api/v1/run`, {
    method: "POST",
    headers: daemonHeaders(authToken, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return RunResponseSchema.parse(j);
}

export const RunAsyncRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  error: z.string().optional(),
});
export type RunAsyncResp = z.infer<typeof RunAsyncRespSchema>;

export const JobRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  status: z.string().optional(), // queued|running|done|error
  error: z.string().optional(),
  created_unix_ms: z.number().optional(),
  updated_unix_ms: z.number().optional(),
  events: z.array(EventSchema).optional(),
  events_cursor_base: z.number().optional(),
  events_cursor_end: z.number().optional(),
  events_cursor_next: z.number().optional(),
  events_reset: z.boolean().optional(),
  result: RunResponseSchema.optional(),
});
export type JobResp = z.infer<typeof JobRespSchema>;

export async function apiRunAsync(base: string, req: RunRequest, authToken?: string): Promise<RunAsyncResp> {
  const payload = RunRequestSchema.parse(req);
  const r = await fetch(`${base}/api/v1/run_async`, {
    method: "POST",
    headers: daemonHeaders(authToken, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return RunAsyncRespSchema.parse(j);
}

export async function apiGetJob(base: string, jobId: string, authToken?: string): Promise<JobResp> {
  const r = await fetch(`${base}/api/v1/job?job_id=${encodeURIComponent(jobId)}`, { headers: daemonHeaders(authToken) });
  const j = await r.json();
  return JobRespSchema.parse(j);
}

export async function apiGetJobProgress(
  base: string,
  jobId: string,
  authToken?: string,
  opts?: { cursor?: number; maxEvents?: number },
): Promise<JobResp> {
  const cursor = opts?.cursor ?? 0;
  const maxEvents = opts?.maxEvents ?? 256;
  const r = await fetch(
    `${base}/api/v1/job?job_id=${encodeURIComponent(jobId)}&include_events=1&cursor=${encodeURIComponent(
      String(cursor),
    )}&max_events=${encodeURIComponent(String(maxEvents))}`,
    { headers: daemonHeaders(authToken) },
  );
  const j = await r.json();
  return JobRespSchema.parse(j);
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function apiRunMaybeAsync(
  base: string,
  req: RunRequest,
  authToken?: string,
  opts?: { pollMs?: number; timeoutMs?: number },
): Promise<RunResponse> {
  const pollMs = opts?.pollMs ?? 500;
  const timeoutMs = opts?.timeoutMs ?? 120_000;
  const started = Date.now();

  // Prefer async when available; fall back to sync if endpoint missing.
  let asyncResp: RunAsyncResp | undefined;
  try {
    asyncResp = await apiRunAsync(base, req, authToken);
  } catch {
    // likely 404 or JSON mismatch; fall back to sync.
    return apiRun(base, req, authToken);
  }

  if (!asyncResp.ok || !asyncResp.job_id) {
    // If daemon reported an error, fall back to sync as best-effort.
    return apiRun(base, req, authToken);
  }

  while (true) {
    if (Date.now() - started > timeoutMs) {
      throw new Error(`Timed out waiting for job ${asyncResp.job_id}`);
    }
    const job = await apiGetJob(base, asyncResp.job_id, authToken);
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
