import { z } from "zod";

export type ApiAuth =
  | { mode: "direct"; token?: string }
  | { mode: "broker"; token?: string; agentdToken?: string; deploymentId?: string };

function normalizeBearerHeader(raw?: string): string | null {
  const s = typeof raw === "string" ? raw.trim() : "";
  if (!s) return null;
  if (/^bearer\\s+/i.test(s)) {
    return `Bearer ${s.replace(/^bearer\\s+/i, "").trim()}`;
  }
  return `Bearer ${s}`;
}

// Request headers for talking to:
// - agentd directly: Authorization: Bearer <agentd_token>
// - broker: Authorization: Bearer <oidc_jwt> and optional X-Agentd-Authorization: Bearer <agentd_token>
export function daemonHeaders(auth?: ApiAuth, extra?: Record<string, string>): Record<string, string> {
  const h: Record<string, string> = { ...(extra ?? {}) };
  const authz = normalizeBearerHeader(auth?.token);
  if (authz) h["Authorization"] = authz;
  if (auth && auth.mode === "broker") {
    const agentd = normalizeBearerHeader(auth.agentdToken);
    if (agentd) h["X-Agentd-Authorization"] = agentd;
    const dep = typeof auth.deploymentId === "string" ? auth.deploymentId.trim() : "";
    if (dep) h["X-Agentd-Deployment"] = dep;
  }
  return h;
}

export const HealthSchema = z.object({
  ok: z.boolean(),
  service: z.string().optional(),
  version: z.string().optional(),
  ready: z.boolean().optional(),
  now_unix_ms: z.number().int().nonnegative().optional(),
  uptime_ms: z.number().int().nonnegative().optional(),
  checks: z
    .object({
      db_open: z.boolean().optional(),
    })
    .optional(),
});
export type Health = z.infer<typeof HealthSchema>;

export const CapsSchema = z
  .object({
    ok: z.boolean(),
    service: z.string().optional(),
    version: z.string().optional(),
    api_version: z.string().optional(),
    now_unix_ms: z.number().int().nonnegative().optional(),
    uptime_ms: z.number().int().nonnegative().optional(),
    features: z.any().optional(),
    limits: z.any().optional(),
  })
  .passthrough();
export type Caps = z.infer<typeof CapsSchema>;

export const DiagnosticsSchema = z
  .object({
    ok: z.boolean(),
    service: z.string().optional(),
    version: z.string().optional(),
    ready: z.boolean().optional(),
    now_unix_ms: z.number().int().nonnegative().optional(),
    uptime_ms: z.number().int().nonnegative().optional(),
    checks: z.any().optional(),
    db: z.any().optional(),
    jobs: z.any().optional(),
    workflows: z.any().optional(),
    warnings: z.array(z.string()).optional(),
  })
  .passthrough();
export type Diagnostics = z.infer<typeof DiagnosticsSchema>;

export const DiagnosticsProvidersSchema = z
  .object({
    ok: z.boolean(),
    service: z.string().optional(),
    version: z.string().optional(),
    now_unix_ms: z.number().int().nonnegative().optional(),
    uptime_ms: z.number().int().nonnegative().optional(),
    providers: z.any().optional(),
  })
  .passthrough();
export type DiagnosticsProviders = z.infer<typeof DiagnosticsProvidersSchema>;

export const DiagnosticsProviderTestReqSchema = z
  .object({
    provider: z.string().min(1),
    base_url: z.string().optional(),
    model: z.string().optional(),
    prompt: z.string().optional(),
    expect: z.string().optional(),
    tools: z.enum(["none", "basic", "host"]).optional(),
    require_tool_call: z.boolean().optional(),
    timeout_ms: z.number().int().positive().optional(),
    max_steps: z.number().int().nonnegative().optional(),
    max_tool_calls_total: z.number().int().nonnegative().optional(),
    max_tool_calls_per_tool: z.number().int().nonnegative().optional(),
    max_tool_call_args_chars: z.number().int().nonnegative().optional(),
    max_repeated_tool_calls: z.number().int().nonnegative().optional(),
    include_run: z.boolean().optional(),
  })
  .passthrough();
export type DiagnosticsProviderTestReq = z.infer<typeof DiagnosticsProviderTestReqSchema>;

export const DiagnosticsProviderTestRespSchema = z
  .object({
    ok: z.boolean(),
    provider: z.string().optional(),
    base_url: z.string().optional(),
    model: z.string().optional(),
    duration_ms: z.number().int().nonnegative().optional(),
    expect: z.string().optional(),
    assistant_text: z.string().optional(),
    error: z.string().optional(),
    http_status: z.number().optional(),
    run: z.any().optional(),
  })
  .passthrough();
export type DiagnosticsProviderTestResp = z.infer<typeof DiagnosticsProviderTestRespSchema>;

export const DaemonConfigSchema = z
  .object({
    ok: z.boolean(),
    service: z.string().optional(),
    version: z.string().optional(),
    have_jsoncpp: z.boolean().optional(),
    daemon: z
      .object({
        listen_host: z.string().optional(),
        listen_port: z.number().optional(),
        db_path: z.string().nullable().optional(),
        state_dir: z.string().nullable().optional(),
        sessions_root_dir: z.string().nullable().optional(),
        max_steps_default: z.number().int().nonnegative().optional(),
        max_tool_calls_total_default: z.number().int().nonnegative().optional(),
        max_tool_calls_per_tool_default: z.number().int().nonnegative().optional(),
        max_tool_call_args_chars_default: z.number().int().nonnegative().optional(),
        tool_call_limits_default: z
          .array(
            z.object({
              tool: z.string(),
              max_calls: z.number().int().nonnegative(),
            })
          )
          .optional(),
        base_url: z.string().optional(),
        model: z.string().optional(),
        summary_model: z.string().nullable().optional(),
        summary_max_chars: z.number().optional(),
        timeout_ms: z.number().optional(),
        proxy_url_set: z.boolean().optional(),
        api_key_set: z.boolean().optional(),
        provider_keys_set: z
          .object({
            deepseek: z.boolean().optional(),
            openrouter: z.boolean().optional(),
            moonshot: z.boolean().optional(),
            openai: z.boolean().optional(),
          })
          .optional(),
        auth_enabled: z.boolean().optional(),
        allow_unauthenticated_non_loopback: z.boolean().optional(),
        auth_cookie_name: z.string().optional(),
      })
      .optional(),
    cors: z
      .object({
        enabled: z.boolean().optional(),
        origins: z.array(z.string()).optional(),
        allow_headers: z.string().optional(),
        allow_methods: z.string().optional(),
        allow_credentials: z.boolean().optional(),
        max_age_seconds: z.number().optional(),
        routes: z
          .array(
            z
              .object({
                path_prefix: z.string().optional(),
                origins: z.array(z.string()).optional(),
                allow_credentials: z.boolean().optional(),
              })
              .passthrough(),
          )
          .optional(),
      })
      .optional(),
    sandbox: z
      .object({
        tools: z.string().optional(),
        host_scope_root: z.string().nullable().optional(),
        yolo_default: z.boolean().optional(),
        host_policy: z.enum(["full", "readonly"]).optional(),
      })
      .optional(),
    jobs: z
      .object({
        job_ttl_ms: z.number().optional(),
        max_jobs: z.number().optional(),
      })
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type DaemonConfigResp = z.infer<typeof DaemonConfigSchema>;

export const DaemonConfigUpdateReqSchema = z
  .object({
    base_url: z.string().optional(),
    model: z.string().optional(),
    summary_model: z.string().nullable().optional(),
    summary_max_chars: z.number().int().nonnegative().optional(),
    max_steps_default: z.number().int().nonnegative().optional(),
    max_tool_calls_total_default: z.number().int().nonnegative().optional(),
    max_tool_calls_per_tool_default: z.number().int().nonnegative().optional(),
    max_tool_call_args_chars_default: z.number().int().nonnegative().optional(),
    tool_call_limits_default: z
      .array(
        z.object({
          tool: z.string(),
          max_calls: z.number().int().nonnegative(),
        })
      )
      .optional(),
    proxy_url: z.string().nullable().optional(),
    timeout_ms: z.number().int().positive().optional(),
    // Either set explicit mapping...
    provider_keys: z.record(z.string(), z.string().nullable()).optional(),
    // ...or set a single key for inferred/explicit provider.
    provider: z.string().optional(),
    api_key: z.string().optional(),
  })
  .passthrough();
export type DaemonConfigUpdateReq = z.infer<typeof DaemonConfigUpdateReqSchema>;

export const DaemonConfigUpdateRespSchema = z
  .object({
    ok: z.boolean(),
    base_url: z.string().optional(),
    model: z.string().optional(),
    summary_model: z.string().nullable().optional(),
    summary_max_chars: z.number().optional(),
    timeout_ms: z.number().optional(),
    max_steps_default: z.number().optional(),
    max_tool_calls_total_default: z.number().optional(),
    max_tool_calls_per_tool_default: z.number().optional(),
    max_tool_call_args_chars_default: z.number().optional(),
    tool_call_limits_default: z
      .array(
        z.object({
          tool: z.string(),
          max_calls: z.number().int().nonnegative(),
        })
      )
      .optional(),
    proxy_url_set: z.boolean().optional(),
    provider_keys_set: z.any().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type DaemonConfigUpdateResp = z.infer<typeof DaemonConfigUpdateRespSchema>;

export async function apiUpdateDaemonConfig(base: string, req: DaemonConfigUpdateReq, auth?: ApiAuth): Promise<DaemonConfigUpdateResp> {
  const payload = DaemonConfigUpdateReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/config/update`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return DaemonConfigUpdateRespSchema.parse(j);
}

export const RunRequestSchema = z.object({
  prompt: z.string().min(1),
  trace_id: z.string().optional(),
  session_id: z.string().optional(),
  no_session: z.boolean().optional(),
  // Optional session-relative attachments uploaded via POST /api/v1/session/upload.
  // For convenience, items may be strings (paths) or objects with metadata.
  input_files: z
    .array(
      z.union([
        z.string(),
        z
          .object({
            path: z.string(),
            name: z.string().optional(),
            mime: z.string().optional(),
            kind: z.string().optional(),
          })
          .passthrough(),
      ]),
    )
    .optional(),
  client: z
    .object({
      id: z.string().optional(),
      kind: z.string().optional(),
      instance_id: z.string().optional(),
    })
    .optional(),
  // When true, the daemon should only mark a run as successful once the client has acknowledged
  // any requested UI actions/artifacts (prevents "false done" reports in interactive UIs).
  require_client_acks: z.boolean().optional(),
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
  host_policy: z.enum(["full", "readonly"]).optional(),
  max_steps: z.number().int().nonnegative().optional(),
  max_repeated_tool_calls: z.number().int().nonnegative().optional(),
  max_tool_calls_total: z.number().int().nonnegative().optional(),
  max_tool_calls_per_tool: z.number().int().nonnegative().optional(),
  max_tool_call_args_chars: z.number().int().nonnegative().optional(),
  tool_call_limits: z
    .array(
      z.object({
        tool: z.string(),
        max_calls: z.number().int().nonnegative(),
      })
    )
    .optional(),
  max_chars: z.number().int().nonnegative().optional(),
  keep_last: z.number().int().nonnegative().optional(),
  memory_context_mode: z.enum(["files", "search", "index", "salience"]).optional(),
  memory_include_structured: z.boolean().optional(),
  memory_include_core: z.boolean().optional(),
  memory_include_daily: z.boolean().optional(),
  memory_include_session: z.boolean().optional(),
  memory_daily_days: z.number().int().nonnegative().optional(),
  memory_total_cap: z.number().int().nonnegative().optional(),
  memory_search_query: z.string().optional(),
  memory_search_use_index: z.boolean().optional(),
  memory_search_case_sensitive: z.boolean().optional(),
  memory_search_fallback_to_files: z.boolean().optional(),
  memory_search_max_results: z.number().int().nonnegative().optional(),
  memory_search_max_snippet_chars: z.number().int().nonnegative().optional(),
  memory_search_context_lines: z.number().int().nonnegative().optional(),
  trace: z.boolean().optional(),
  yolo: z.boolean().optional(),
  verbose: z.boolean().optional(),
});
export type RunRequest = z.infer<typeof RunRequestSchema>;

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

export const SessionUploadReqSchema = z
  .object({
    session_id: z.string().min(1),
    files: z
      .array(
        z.object({
          name: z.string().min(1),
          mime: z.string().optional(),
          data_base64: z.string().min(1),
        }),
      )
      .min(1),
  })
  .passthrough();
export type SessionUploadReq = z.infer<typeof SessionUploadReqSchema>;

export const SessionUploadRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    files: z
      .array(
        z
          .object({
            name: z.string().optional(),
            mime: z.string().optional(),
            kind: z.string().optional(),
            path: z.string().optional(),
            bytes: z.number().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type SessionUploadResp = z.infer<typeof SessionUploadRespSchema>;

export async function apiPostSessionUpload(base: string, req: SessionUploadReq, auth?: ApiAuth): Promise<SessionUploadResp> {
  const payload = SessionUploadReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/session/upload`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return SessionUploadRespSchema.parse(j);
}

export const EventSchema = z.object({
  type: z.string(),
  trace_id: z.string().optional(),
  data: z.any().optional(),
});
export type AgentEvent = z.infer<typeof EventSchema>;

export const RunResponseSchema = z.object({
  ok: z.boolean(),
  trace_id: z.string().optional(),
  assistant_text: z.string().optional(),
  error: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
  trace_text: z.string().optional(),
  effective_yolo: z.boolean().optional(),
  effective_host_policy: z.enum(["full", "readonly"]).optional(),
  effective_timeout_ms: z.number().optional(),
  effective_max_tool_call_args_chars: z.number().optional(),
  effective_stream_assistant: z.boolean().optional(),
  verbose: z.boolean().optional(),
  events: z.array(EventSchema).optional(),
});
export type RunResponse = z.infer<typeof RunResponseSchema>;

export const ToolDefsRespSchema = z.object({
  ok: z.boolean(),
  tools: z.string().optional(),
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
  auth?: ApiAuth,
  opts?: { tools?: "host" | "basic" | "none"; yolo?: boolean; hostPolicy?: "full" | "readonly"; sessionId?: string },
): Promise<ToolDefsResp> {
  const q = new URLSearchParams();
  if (opts?.tools) q.set("tools", opts.tools);
  if (typeof opts?.yolo === "boolean") q.set("yolo", opts.yolo ? "1" : "0");
  if (opts?.hostPolicy) q.set("host_policy", opts.hostPolicy);
  if (typeof opts?.sessionId === "string" && opts.sessionId.length > 0) q.set("session_id", opts.sessionId);
  const r = await fetch(`${base}/api/v1/tools?${q.toString()}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return ToolDefsRespSchema.parse(j);
}

export async function apiCancelJob(base: string, jobId: string, auth?: ApiAuth): Promise<any> {
  const r = await fetch(`${base}/api/v1/job/cancel?job_id=${encodeURIComponent(jobId)}`, {
    method: "POST",
    headers: daemonHeaders(auth),
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
    daemonAuth?: ApiAuth;
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

  const headers: Record<string, string> = daemonHeaders(opts.daemonAuth);
  if (opts.apiKey && opts.apiKey.trim().length > 0) {
    headers["X-OpenRouter-Key"] = opts.apiKey.trim();
  }
  const r = await fetch(`${base}/api/v1/openrouter/models?${q.toString()}`, { headers });
  const j = await r.json();
  return OpenRouterModelsRespSchema.parse(j);
}

export async function apiGetHealth(base: string, auth?: ApiAuth): Promise<Health> {
  const r = await fetch(`${base}/api/v1/health`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return HealthSchema.parse(j);
}

export async function apiGetCaps(base: string, auth?: ApiAuth): Promise<Caps> {
  const r = await fetch(`${base}/api/v1/caps`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return CapsSchema.parse(j);
}

export async function apiGetConfig(base: string, auth?: ApiAuth): Promise<DaemonConfigResp> {
  const r = await fetch(`${base}/api/v1/config`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DaemonConfigSchema.parse(j);
}

export async function apiGetDiagnostics(base: string, auth?: ApiAuth): Promise<Diagnostics> {
  const r = await fetch(`${base}/api/v1/diagnostics`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DiagnosticsSchema.parse(j);
}

export async function apiGetDiagnosticsProviders(base: string, auth?: ApiAuth): Promise<DiagnosticsProviders> {
  const r = await fetch(`${base}/api/v1/diagnostics/providers`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DiagnosticsProvidersSchema.parse(j);
}

export async function apiPostDiagnosticsProviderTest(
  base: string,
  req: DiagnosticsProviderTestReq,
  auth?: ApiAuth,
): Promise<DiagnosticsProviderTestResp> {
  const payload = DiagnosticsProviderTestReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/diagnostics/provider_test`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return DiagnosticsProviderTestRespSchema.parse(j);
}

export const SessionsSchema = z.object({
  ok: z.boolean(),
  sessions: z.array(z.string()).optional(),
  error: z.string().optional(),
});
export type SessionsResp = z.infer<typeof SessionsSchema>;

export async function apiListSessions(base: string, auth?: ApiAuth): Promise<SessionsResp> {
  const r = await fetch(`${base}/api/v1/sessions`, { headers: daemonHeaders(auth) });
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

export async function apiGetSession(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return SessionSchema.parse(j);
}

export const SessionSceneSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    updated_unix_ms: z.number().optional(),
    scene: z.record(z.any()).optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type SessionSceneResp = z.infer<typeof SessionSceneSchema>;

export async function apiGetSessionScene(base: string, sessionId: string, auth?: ApiAuth): Promise<SessionSceneResp> {
  const r = await fetch(`${base}/api/v1/session/scene?session_id=${encodeURIComponent(sessionId)}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return SessionSceneSchema.parse(j);
}

export const SessionSceneApplyReqSchema = z.object({
  session_id: z.string().min(1),
  ops: z.array(z.any()),
});
export type SessionSceneApplyReq = z.infer<typeof SessionSceneApplyReqSchema>;

export const SessionSceneApplyRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    updated_unix_ms: z.number().optional(),
    apply: z.any().optional(),
    scene: z.record(z.any()).optional(),
    warning: z.string().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type SessionSceneApplyResp = z.infer<typeof SessionSceneApplyRespSchema>;

export async function apiPostSessionSceneApply(
  base: string,
  req: SessionSceneApplyReq,
  auth?: ApiAuth,
): Promise<SessionSceneApplyResp> {
  const payload = SessionSceneApplyReqSchema.parse(req);
  const r = await fetch(`${base}/api/v1/session/scene/apply`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return SessionSceneApplyRespSchema.parse(j);
}

export const SessionUiEventReqSchema = z.object({
  session_id: z.string().min(1),
  type: z.string().min(1),
  ts_unix_ms: z.number().optional(),
  // Optional client identity (collaboration protocol).
  client: z
    .object({
      id: z.string().optional(),
      kind: z.string().optional(),
      instance_id: z.string().optional(),
    })
    .optional(),
  data: z.any().optional(),
  append_to_session: z.boolean().optional(),
});
export type SessionUiEventReq = z.infer<typeof SessionUiEventReqSchema>;

export const SessionUiEventRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    type: z.string().optional(),
    appended_to_session: z.boolean().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type SessionUiEventResp = z.infer<typeof SessionUiEventRespSchema>;

export async function apiPostSessionUiEvent(
  base: string,
  req: SessionUiEventReq,
  auth?: ApiAuth,
): Promise<SessionUiEventResp> {
  const payload = SessionUiEventReqSchema.parse(req);
  // Preferred endpoint name is /session/client_event; /session/ui_event remains as a legacy alias.
  const r = await fetch(`${base}/api/v1/session/client_event`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return SessionUiEventRespSchema.parse(j);
}

export const NewSessionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    created: z.boolean().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type NewSessionResp = z.infer<typeof NewSessionRespSchema>;

export async function apiNewSession(
  base: string,
  auth?: ApiAuth,
  opts?: { sessionId?: string; createFiles?: boolean },
): Promise<NewSessionResp> {
  const payload: any = {};
  if (opts?.sessionId) payload.session_id = String(opts.sessionId);
  if (typeof opts?.createFiles === "boolean") payload.create_files = opts.createFiles;
  const r = await fetch(`${base}/api/v1/session/new`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(payload),
  });
  const j = await r.json();
  return NewSessionRespSchema.parse(j);
}

export const DeleteSessionRespSchema = z
  .object({
    ok: z.boolean(),
    session_id: z.string().optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type DeleteSessionResp = z.infer<typeof DeleteSessionRespSchema>;

export async function apiDeleteSession(base: string, sessionId: string, auth?: ApiAuth): Promise<DeleteSessionResp> {
  const r = await fetch(`${base}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return DeleteSessionRespSchema.parse(j);
}

export const AuditSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  entries: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type AuditResp = z.infer<typeof AuditSchema>;

export async function apiGetAudit(base: string, sessionId: string, auth?: ApiAuth): Promise<AuditResp> {
  const r = await fetch(`${base}/api/v1/session/audit?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return AuditSchema.parse(j);
}

export const SessionClientEventsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  max_bytes: z.number().optional(),
  count: z.number().optional(),
  events: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type SessionClientEventsResp = z.infer<typeof SessionClientEventsSchema>;

export async function apiGetSessionClientEvents(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number },
): Promise<SessionClientEventsResp> {
  const maxBytes = typeof opts?.maxBytes === "number" ? opts.maxBytes : 1024 * 1024;
  const r = await fetch(
    `${base}/api/v1/session/client_events?session_id=${encodeURIComponent(sessionId)}&max_bytes=${encodeURIComponent(String(maxBytes))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return SessionClientEventsSchema.parse(j);
}

export const SessionArtifactsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  count: z.number().optional(),
  artifacts: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type SessionArtifactsResp = z.infer<typeof SessionArtifactsSchema>;

export const DbRunsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  runs: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbRunsResp = z.infer<typeof DbRunsSchema>;

export const DbRunSchema = z.object({
  ok: z.boolean(),
  run: z.any().optional(),
  events: z.array(z.any()).optional(),
  tool_records: z.array(z.any()).optional(),
  artifacts: z.array(z.any()).optional(),
  ui_actions: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbRunResp = z.infer<typeof DbRunSchema>;

export const DbArtifactsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  artifacts: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbArtifactsResp = z.infer<typeof DbArtifactsSchema>;

export const DbUiActionsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  ui_actions: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbUiActionsResp = z.infer<typeof DbUiActionsSchema>;

export const DbSessionsSchema = z.object({
  ok: z.boolean(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  sessions: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbSessionsResp = z.infer<typeof DbSessionsSchema>;

export const DbMessagesSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  max_content_bytes: z.number().optional(),
  count: z.number().optional(),
  messages: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbMessagesResp = z.infer<typeof DbMessagesSchema>;

export const DbClientEventsSchema = z.object({
  ok: z.boolean(),
  session_id: z.string().optional(),
  limit: z.number().optional(),
  offset: z.number().optional(),
  count: z.number().optional(),
  client_events: z.array(z.any()).optional(),
  error: z.string().optional(),
});
export type DbClientEventsResp = z.infer<typeof DbClientEventsSchema>;

export async function apiGetSessionArtifacts(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { maxBytes?: number; maxArtifacts?: number },
): Promise<SessionArtifactsResp> {
  const maxBytes = opts?.maxBytes ?? 2 * 1024 * 1024;
  const maxArtifacts = opts?.maxArtifacts ?? 64;
  const r = await fetch(
    `${base}/api/v1/session/artifacts?session_id=${encodeURIComponent(sessionId)}&max_bytes=${encodeURIComponent(
      String(maxBytes),
    )}&max_artifacts=${encodeURIComponent(String(maxArtifacts))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return SessionArtifactsSchema.parse(j);
}

export async function apiGetDbRuns(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; onlyErrors?: boolean; stopReason?: string },
): Promise<DbRunsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const onlyErrors = !!opts?.onlyErrors;
  const stopReason = typeof opts?.stopReason === "string" ? opts.stopReason : "";
  const r = await fetch(
    `${base}/api/v1/db/runs?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}&only_errors=${encodeURIComponent(onlyErrors ? "1" : "0")}${
      stopReason.trim().length > 0 ? `&stop_reason=${encodeURIComponent(stopReason.trim())}` : ""
    }`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbRunsSchema.parse(j);
}

export async function apiGetDbRun(
  base: string,
  runId: number,
  auth?: ApiAuth,
  opts?: { includeEvents?: boolean; includeTools?: boolean; includeArtifacts?: boolean; includeUiActions?: boolean },
): Promise<DbRunResp> {
  const q = new URLSearchParams();
  q.set("run_id", String(runId));
  if (opts?.includeEvents) q.set("include_events", "1");
  if (opts?.includeTools) q.set("include_tools", "1");
  if (opts?.includeArtifacts) q.set("include_artifacts", "1");
  if (opts?.includeUiActions) q.set("include_ui_actions", "1");
  const r = await fetch(`${base}/api/v1/db/run?${q.toString()}`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return DbRunSchema.parse(j);
}

export async function apiGetDbArtifacts(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbArtifactsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/artifacts?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbArtifactsSchema.parse(j);
}

export async function apiGetDbUiActions(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbUiActionsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/ui_actions?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbUiActionsSchema.parse(j);
}

export async function apiGetDbSessions(
  base: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbSessionsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/sessions?limit=${encodeURIComponent(String(limit))}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbSessionsSchema.parse(j);
}

export async function apiGetDbMessages(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number; maxContentBytes?: number },
): Promise<DbMessagesResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const maxContentBytes = typeof opts?.maxContentBytes === "number" ? opts.maxContentBytes : 8192;
  const r = await fetch(
    `${base}/api/v1/db/messages?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}&max_content_bytes=${encodeURIComponent(String(maxContentBytes))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbMessagesSchema.parse(j);
}

export async function apiGetDbClientEvents(
  base: string,
  sessionId: string,
  auth?: ApiAuth,
  opts?: { limit?: number; offset?: number },
): Promise<DbClientEventsResp> {
  const limit = typeof opts?.limit === "number" ? opts.limit : 50;
  const offset = typeof opts?.offset === "number" ? opts.offset : 0;
  const r = await fetch(
    `${base}/api/v1/db/client_events?session_id=${encodeURIComponent(sessionId)}&limit=${encodeURIComponent(
      String(limit),
    )}&offset=${encodeURIComponent(String(offset))}`,
    { headers: daemonHeaders(auth) },
  );
  const j = await r.json();
  return DbClientEventsSchema.parse(j);
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

export const RunAsyncRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  trace_id: z.string().optional(),
  error: z.string().optional(),
});
export type RunAsyncResp = z.infer<typeof RunAsyncRespSchema>;

export const JobRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  trace_id: z.string().optional(),
  status: z.string().optional(), // queued|running|done|error|cancelled
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

// Broker (control plane) helpers.
export const BrokerAgentsRespSchema = z
  .object({
    ok: z.boolean(),
    agents: z
      .array(
        z
          .object({
            agent_id: z.string(),
            enabled: z.boolean().optional(),
            created_unix_ms: z.number().optional(),
            owner_sub: z.string().optional(),
            connected: z.boolean().optional(),
            connected_unix_ms: z.number().optional(),
            last_seen_unix_ms: z.number().optional(),
            remote_addr: z.string().optional(),
            labels: z.record(z.any()).optional(),
            meta: z.record(z.any()).optional(),
            deployments: z
              .array(
                z
                  .object({
                    deployment_id: z.string().optional(),
                    connected: z.boolean().optional(),
                    connected_unix_ms: z.number().optional(),
                    last_seen_unix_ms: z.number().optional(),
                    remote_addr: z.string().optional(),
                    meta: z.record(z.any()).optional(),
                  })
                  .passthrough(),
              )
              .optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerAgentsResp = z.infer<typeof BrokerAgentsRespSchema>;

export async function apiBrokerListAgents(brokerBase: string, auth?: ApiAuth): Promise<BrokerAgentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const r = await fetch(`${base}/v1/agents`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerAgentsRespSchema.parse(j);
}

export const BrokerDeploymentsRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    default_deployment_id: z.string().optional(),
    deployments: z
      .array(
        z
          .object({
            deployment_id: z.string().optional(),
            connected: z.boolean().optional(),
            connected_unix_ms: z.number().optional(),
            last_seen_unix_ms: z.number().optional(),
            remote_addr: z.string().optional(),
            meta: z.record(z.any()).optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerDeploymentsResp = z.infer<typeof BrokerDeploymentsRespSchema>;

export async function apiBrokerListDeployments(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
): Promise<BrokerDeploymentsResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/deployments`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerDeploymentsRespSchema.parse(j);
}

export async function apiBrokerProxyJson(
  brokerBase: string,
  agentId: string,
  path: string,
  method: string,
  body: unknown,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const p = path.startsWith("/") ? path : `/${path}`;
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const dep = typeof deploymentId === "string" ? deploymentId.trim() : "";
  if (dep) headers["X-Agentd-Deployment"] = dep;
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/proxy${p}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerOtaUpdate(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/update", "POST", body, auth, deploymentId);
}

export async function apiBrokerOtaUpdateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...body };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/ota/update`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerOtaStatus(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentId?: string,
): Promise<{ status: number; data: any }> {
  return apiBrokerProxyJson(brokerBase, agentId, "/api/v1/ota/status", "GET", undefined, auth, deploymentId);
}

export async function apiBrokerOtaStatusBulk(
  brokerBase: string,
  agentId: string,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const params = new URLSearchParams();
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    params.set("deployment_ids", deploymentIds.join(","));
  }
  const qs = params.toString();
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/ota/status${qs ? `?${qs}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRetentionBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...(body ?? {}) };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/memory/retention/enforce`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsListBulk(
  brokerBase: string,
  agentId: string,
  params: MemoryRecapsListParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "include_summary", params.includeSummary);
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    qs.set("deployment_ids", deploymentIds.join(","));
  }
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/memory/recaps${qs.toString() ? `?${qs.toString()}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemorySalienceBulk(
  brokerBase: string,
  agentId: string,
  params: MemorySalienceParams,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "daily_days", params.dailyDays);
  addQueryParam(qs, "max_items", params.maxItems);
  addQueryParam(qs, "max_structured_items", params.maxStructuredItems);
  addQueryParam(qs, "max_daily_items", params.maxDailyItems);
  addQueryParam(qs, "half_life_days", params.halfLifeDays);
  addQueryParam(qs, "importance_weight", params.importanceWeight);
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    qs.set("deployment_ids", deploymentIds.join(","));
  }
  const url = `${base}/v1/agents/${encodeURIComponent(id)}/memory/salience${qs.toString() ? `?${qs.toString()}` : ""}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export async function apiBrokerMemoryRecapsCreateBulk(
  brokerBase: string,
  agentId: string,
  body: Record<string, any>,
  auth?: ApiAuth,
  deploymentIds?: string[],
): Promise<{ status: number; data: any }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const headers = daemonHeaders(auth, { "Content-Type": "application/json" });
  const payload: Record<string, any> = { ...(body ?? {}) };
  if (Array.isArray(deploymentIds) && deploymentIds.length > 0) {
    payload.deployment_ids = deploymentIds;
  }
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/memory/recaps`, {
    method: "POST",
    headers,
    body: JSON.stringify(payload),
  });
  let data: any = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  return { status: r.status, data };
}

export const BrokerMembersRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    owner_sub: z.string().optional(),
    members: z
      .array(
        z
          .object({
            user_sub: z.string(),
            role: z.string().optional(),
            created_unix_ms: z.number().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerMembersResp = z.infer<typeof BrokerMembersRespSchema>;

export async function apiBrokerGetMembers(brokerBase: string, agentId: string, auth?: ApiAuth): Promise<BrokerMembersResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerMembersRespSchema.parse(j);
}

export async function apiBrokerUpsertMember(
  brokerBase: string,
  agentId: string,
  req: { user_sub: string; role?: string },
  auth?: ApiAuth,
): Promise<{ ok: boolean; error?: string }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify({
      user_sub: String(req?.user_sub || "").trim(),
      role: String(req?.role || "").trim(),
    }),
  });
  const j = await r.json();
  if (!j || typeof j !== "object") throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: String((j as any).error || (j as any).err || "request failed") };
}

export async function apiBrokerDeleteMember(
  brokerBase: string,
  agentId: string,
  userSub: string,
  auth?: ApiAuth,
): Promise<{ ok: boolean; error?: string }> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const sub = String(userSub || "").trim();
  if (!sub) throw new Error("missing user_sub");
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/members/${encodeURIComponent(sub)}`, {
    method: "DELETE",
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  if (!j || typeof j !== "object") throw new Error("bad json");
  if (j.ok === true) return { ok: true };
  return { ok: false, error: String((j as any).error || (j as any).err || "request failed") };
}

export const BrokerMembershipAuditRespSchema = z
  .object({
    ok: z.boolean(),
    agent_id: z.string().optional(),
    owner_sub: z.string().optional(),
    audit: z
      .array(
        z
          .object({
            ts_unix_ms: z.number().optional(),
            actor_sub: z.string().optional(),
            target_sub: z.string().optional(),
            action: z.string().optional(),
            role: z.string().optional(),
            trace_id: z.string().optional(),
          })
          .passthrough(),
      )
      .optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerMembershipAuditResp = z.infer<typeof BrokerMembershipAuditRespSchema>;

export async function apiBrokerGetMembershipAudit(
  brokerBase: string,
  agentId: string,
  limit: number,
  auth?: ApiAuth,
): Promise<BrokerMembershipAuditResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const id = String(agentId || "").trim();
  if (!id) throw new Error("missing agent_id");
  const lim = Number.isFinite(limit) ? Math.max(1, Math.min(limit, 500)) : 200;
  const r = await fetch(`${base}/v1/agents/${encodeURIComponent(id)}/membership_audit?limit=${encodeURIComponent(String(lim))}`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return BrokerMembershipAuditRespSchema.parse(j);
}

export const AgentdTraceRespSchema = z
  .object({
    ok: z.boolean(),
    trace_id: z.string().optional(),
    count: z.number().optional(),
    records: z.array(z.any()).optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type AgentdTraceResp = z.infer<typeof AgentdTraceRespSchema>;

export async function apiAgentdTrace(base: string, traceId: string, auth?: ApiAuth): Promise<AgentdTraceResp> {
  const tid = String(traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const r = await fetch(`${base}/api/v1/trace?trace_id=${encodeURIComponent(tid)}&limit=200&max_bytes=1048576`, {
    headers: daemonHeaders(auth),
  });
  const j = await r.json();
  return AgentdTraceRespSchema.parse(j);
}

export const BrokerTraceRespSchema = z
  .object({
    ok: z.boolean(),
    trace_id: z.string().optional(),
    orchestrate: z.array(z.any()).optional(),
    relay_audit: z.array(z.any()).optional(),
    agentd: z.array(z.any()).optional(),
    error: z.string().optional(),
  })
  .passthrough();
export type BrokerTraceResp = z.infer<typeof BrokerTraceRespSchema>;

export async function apiBrokerTrace(brokerBase: string, traceId: string, auth?: ApiAuth): Promise<BrokerTraceResp> {
  const base = brokerBase.replace(/\/+$/, "");
  const tid = String(traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const r = await fetch(`${base}/v1/trace?trace_id=${encodeURIComponent(tid)}&limit=200&fanout=1`, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return BrokerTraceRespSchema.parse(j);
}

export const MemoryQueryRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryQueryResp = z.infer<typeof MemoryQueryRespSchema>;

export const MemoryCorrelateRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryCorrelateResp = z.infer<typeof MemoryCorrelateRespSchema>;

export const MemoryCheckpointsRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryCheckpointsResp = z.infer<typeof MemoryCheckpointsRespSchema>;

export const MemoryIndexRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryIndexResp = z.infer<typeof MemoryIndexRespSchema>;

export const MemorySalienceRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemorySalienceResp = z.infer<typeof MemorySalienceRespSchema>;

export const MemoryRetentionRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryRetentionResp = z.infer<typeof MemoryRetentionRespSchema>;

export const MemoryRecapsRespSchema = z
  .object({
    ok: z.boolean(),
    error: z.string().optional(),
  })
  .passthrough();
export type MemoryRecapsResp = z.infer<typeof MemoryRecapsRespSchema>;

type MemoryQueryParams = {
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  keyPrefix?: string;
  limit?: number;
};

type MemoryCorrelateParams = {
  traceId: string;
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  keyPrefix?: string;
  maxEntries?: number;
  timeline?: boolean;
};

type MemoryCheckpointsParams = {
  sinceUtcMs?: number;
  untilUtcMs?: number;
  structuredPath?: string;
  limit?: number;
};

type MemoryIndexParams = {
  sessionId?: string;
  includeStructured?: boolean;
  includeCore?: boolean;
  includeDaily?: boolean;
  includeSession?: boolean;
  dailyDays?: number;
};

type MemorySalienceParams = {
  includeStructured?: boolean;
  includeDaily?: boolean;
  dailyDays?: number;
  maxItems?: number;
  maxStructuredItems?: number;
  maxDailyItems?: number;
  halfLifeDays?: number;
  importanceWeight?: number;
};

type MemoryRecapsListParams = {
  limit?: number;
  includeSummary?: boolean;
};

const addQueryParam = (params: URLSearchParams, key: string, value?: string | number | boolean) => {
  if (value === undefined || value === null) return;
  const s = typeof value === "string" ? value.trim() : String(value);
  if (!s) return;
  params.set(key, s);
};

export async function apiMemoryQuery(base: string, params: MemoryQueryParams, auth?: ApiAuth): Promise<MemoryQueryResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "key_prefix", params.keyPrefix);
  addQueryParam(qs, "limit", params.limit);
  const url = qs.toString() ? `${base}/api/v1/memory/query?${qs.toString()}` : `${base}/api/v1/memory/query`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemoryQueryRespSchema.parse(j);
}

export async function apiMemoryCorrelate(
  base: string,
  params: MemoryCorrelateParams,
  auth?: ApiAuth,
): Promise<MemoryCorrelateResp> {
  const tid = String(params.traceId || "").trim();
  if (!tid) throw new Error("missing trace_id");
  const qs = new URLSearchParams();
  addQueryParam(qs, "trace_id", tid);
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "key_prefix", params.keyPrefix);
  addQueryParam(qs, "max_entries", params.maxEntries);
  if (params.timeline) addQueryParam(qs, "timeline", "1");
  const url = `${base}/api/v1/memory/correlate?${qs.toString()}`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemoryCorrelateRespSchema.parse(j);
}

export async function apiMemoryCheckpoints(
  base: string,
  params: MemoryCheckpointsParams,
  auth?: ApiAuth,
): Promise<MemoryCheckpointsResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "since_utc_ms", params.sinceUtcMs);
  addQueryParam(qs, "until_utc_ms", params.untilUtcMs);
  addQueryParam(qs, "structured_path", params.structuredPath);
  addQueryParam(qs, "limit", params.limit);
  const url = qs.toString() ? `${base}/api/v1/memory/checkpoints?${qs.toString()}` : `${base}/api/v1/memory/checkpoints`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemoryCheckpointsRespSchema.parse(j);
}

export async function apiMemoryIndex(
  base: string,
  params: MemoryIndexParams,
  auth?: ApiAuth,
): Promise<MemoryIndexResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "session_id", params.sessionId);
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_core", params.includeCore);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "include_session", params.includeSession);
  addQueryParam(qs, "daily_days", params.dailyDays);
  const url = qs.toString() ? `${base}/api/v1/memory/index?${qs.toString()}` : `${base}/api/v1/memory/index`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemoryIndexRespSchema.parse(j);
}

export async function apiMemorySalience(
  base: string,
  params: MemorySalienceParams,
  auth?: ApiAuth,
): Promise<MemorySalienceResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "include_structured", params.includeStructured);
  addQueryParam(qs, "include_daily", params.includeDaily);
  addQueryParam(qs, "daily_days", params.dailyDays);
  addQueryParam(qs, "max_items", params.maxItems);
  addQueryParam(qs, "max_structured_items", params.maxStructuredItems);
  addQueryParam(qs, "max_daily_items", params.maxDailyItems);
  addQueryParam(qs, "half_life_days", params.halfLifeDays);
  addQueryParam(qs, "importance_weight", params.importanceWeight);
  const url = qs.toString() ? `${base}/api/v1/memory/salience?${qs.toString()}` : `${base}/api/v1/memory/salience`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemorySalienceRespSchema.parse(j);
}

export async function apiMemoryRetentionEnforce(
  base: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<MemoryRetentionResp> {
  const r = await fetch(`${base}/api/v1/memory/retention/enforce`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(body ?? {}),
  });
  const j = await r.json();
  return MemoryRetentionRespSchema.parse(j);
}

export async function apiMemoryRecapsList(
  base: string,
  params: MemoryRecapsListParams,
  auth?: ApiAuth,
): Promise<MemoryRecapsResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "limit", params.limit);
  addQueryParam(qs, "include_summary", params.includeSummary);
  const url = qs.toString() ? `${base}/api/v1/memory/recaps?${qs.toString()}` : `${base}/api/v1/memory/recaps`;
  const r = await fetch(url, { headers: daemonHeaders(auth) });
  const j = await r.json();
  return MemoryRecapsRespSchema.parse(j);
}

export async function apiMemoryRecapsCreate(
  base: string,
  body: Record<string, any>,
  auth?: ApiAuth,
): Promise<MemoryRecapsResp> {
  const r = await fetch(`${base}/api/v1/memory/recaps`, {
    method: "POST",
    headers: daemonHeaders(auth, { "Content-Type": "application/json" }),
    body: JSON.stringify(body ?? {}),
  });
  const j = await r.json();
  return MemoryRecapsRespSchema.parse(j);
}
