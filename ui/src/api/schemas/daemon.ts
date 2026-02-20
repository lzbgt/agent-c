import { z } from "zod";

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
    err: z.string().optional(),
    code: z.string().optional(),
    http_status: z.number().optional(),
    run: z.any().optional(),
  })
  .passthrough();
export type DiagnosticsProviderTestResp = z.infer<typeof DiagnosticsProviderTestRespSchema>;

export const ClientPrefsSchema = z
  .object({
    ok: z.boolean(),
    found: z.boolean().optional(),
    client_id: z.string().optional(),
    client_kind: z.string().optional(),
    version: z.number().int().optional(),
    updated_utc_ms: z.number().int().nonnegative().optional(),
    prefs: z.any().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type ClientPrefs = z.infer<typeof ClientPrefsSchema>;

export const ClientPrefsUpdateReqSchema = z
  .object({
    client_id: z.string().min(1),
    client_kind: z.string().optional(),
    prefs: z.any(),
  })
  .passthrough();
export type ClientPrefsUpdateReq = z.infer<typeof ClientPrefsUpdateReqSchema>;

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
            }),
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
    err: z.string().optional(),
    code: z.string().optional(),
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
        }),
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
        }),
      )
      .optional(),
    proxy_url_set: z.boolean().optional(),
    provider_keys_set: z.any().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type DaemonConfigUpdateResp = z.infer<typeof DaemonConfigUpdateRespSchema>;
