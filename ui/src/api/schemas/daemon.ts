import { z } from "zod";
import type { components as AgentdComponents, paths as AgentdPaths } from "../generated/agentd-openapi";

export type Health = AgentdPaths["/api/v1/health"]["get"]["responses"][200]["content"]["application/json"];
export type Caps = AgentdComponents["schemas"]["CapsResponse"];
export type Diagnostics = AgentdComponents["schemas"]["DiagnosticsResponse"];
export type DiagnosticsProviders = AgentdComponents["schemas"]["DiagnosticsProvidersResponse"];
export type DiagnosticsProviderTestReq = AgentdComponents["schemas"]["DiagnosticsProviderTestRequest"];
export type SandboxMountValidateReq = AgentdComponents["schemas"]["SandboxMountValidateRequest"];
export type SandboxMountValidateResp = AgentdComponents["schemas"]["SandboxMountValidateResponse"];
type ClientPrefsConnectionProfile = AgentdComponents["schemas"]["ClientPrefsConnectionProfile"];
type ClientPrefsConnection = AgentdComponents["schemas"]["ClientPrefsConnection"];
type ClientPrefsPayload = AgentdComponents["schemas"]["ClientPrefs"];
export type ClientPrefs = AgentdComponents["schemas"]["ClientPrefsResponse"];
export type ClientPrefsUpdateReq = AgentdComponents["schemas"]["ClientPrefsUpdateRequest"];

const UnknownRecordSchema = z.record(z.string(), z.unknown());
const ProviderKeysSetSchema = z
  .object({
    deepseek: z.boolean().optional(),
    openrouter: z.boolean().optional(),
    moonshot: z.boolean().optional(),
    openai: z.boolean().optional(),
  })
  .catchall(z.unknown());
const DaemonMemoryConfigSchema = z
  .object({
    consolidate_interval_ms: z.number().int().nonnegative().optional(),
    consolidate_daily_days: z.number().int().nonnegative().optional(),
    consolidate_keep_checkpoints: z.number().int().nonnegative().optional(),
    recap_daily_interval_ms: z.number().int().nonnegative().optional(),
    recap_weekly_interval_ms: z.number().int().nonnegative().optional(),
    recap_daily_days: z.number().int().nonnegative().optional(),
    recap_weekly_days: z.number().int().nonnegative().optional(),
    retention_interval_ms: z.number().int().nonnegative().optional(),
    retention_daily_max_days: z.number().int().nonnegative().optional(),
    retention_daily_max_bytes: z.number().int().nonnegative().optional(),
    retention_checkpoint_max_days: z.number().int().nonnegative().optional(),
    retention_checkpoint_max_count: z.number().int().nonnegative().optional(),
    retention_structured_deprecate_days: z.number().int().nonnegative().optional(),
    retention_structured_deprecate_max_entries: z.number().int().nonnegative().optional(),
    salience_daily_days: z.number().int().nonnegative().optional(),
    salience_max_items: z.number().int().nonnegative().optional(),
    salience_structured_max_items: z.number().int().nonnegative().optional(),
    salience_daily_max_items: z.number().int().nonnegative().optional(),
    salience_half_life_days: z.number().nonnegative().optional(),
    salience_importance_weight: z.number().nonnegative().optional(),
  })
  .passthrough();

export const HealthSchema: z.ZodType<Health> = z.object({
  ok: z.boolean(),
  service: z.string(),
  version: z.string(),
  ready: z.boolean().optional(),
  now_unix_ms: z.number().int().nonnegative().optional(),
  uptime_ms: z.number().int().nonnegative().optional(),
  checks: z
    .object({
      db_open: z.boolean().optional(),
    })
    .optional(),
});

export const CapsSchema: z.ZodType<Caps> = z
  .object({
    ok: z.boolean(),
    service: z.string(),
    version: z.string(),
    api_version: z.string(),
    now_unix_ms: z.number().int().nonnegative().optional(),
    uptime_ms: z.number().int().nonnegative().optional(),
    features: UnknownRecordSchema.optional(),
    limits: UnknownRecordSchema.optional(),
  })
  .passthrough();

export const DiagnosticsSchema: z.ZodType<Diagnostics> = z
  .object({
    ok: z.boolean(),
    service: z.string(),
    version: z.string(),
    ready: z.boolean(),
    now_unix_ms: z.number().int().nonnegative(),
    uptime_ms: z.number().int().nonnegative(),
    checks: UnknownRecordSchema.optional(),
    db: UnknownRecordSchema.optional(),
    sandbox_mount_allowlist: UnknownRecordSchema.optional(),
    jobs: UnknownRecordSchema.optional(),
    workflows: UnknownRecordSchema.optional(),
    warnings: z.array(z.string()).optional(),
  })
  .passthrough();

export const DiagnosticsProvidersSchema: z.ZodType<DiagnosticsProviders> = z
  .object({
    ok: z.boolean(),
    service: z.string(),
    version: z.string(),
    now_unix_ms: z.number().int().nonnegative(),
    uptime_ms: z.number().int().nonnegative(),
    providers: UnknownRecordSchema,
  })
  .passthrough();

export const DiagnosticsProviderTestReqSchema: z.ZodType<DiagnosticsProviderTestReq> = z
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
    max_tool_result_chars: z.number().int().nonnegative().optional(),
    max_repeated_tool_calls: z.number().int().nonnegative().optional(),
    include_run: z.boolean().optional(),
  })
  .passthrough();

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
    run: UnknownRecordSchema.optional(),
  })
  .passthrough();
export type DiagnosticsProviderTestResp = z.infer<typeof DiagnosticsProviderTestRespSchema>;

export const SandboxMountValidateReqSchema: z.ZodType<SandboxMountValidateReq> = z
  .object({
    host_path: z.string().min(1),
    container_path: z.string().min(1),
    container_prefix: z.string().optional(),
    is_main: z.boolean().optional(),
  })
  .passthrough();

export const SandboxMountValidateRespSchema: z.ZodType<SandboxMountValidateResp> = z
  .object({
    ok: z.boolean(),
    allowed: z.boolean(),
    readonly: z.boolean(),
    reason: z.string(),
    resolved_host_path: z.string().optional(),
    resolved_container_path: z.string().optional(),
    matched_root: z.string().optional(),
    blocked_pattern: z.string().optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

const ClientPrefsConnectionProfileSchema: z.ZodType<ClientPrefsConnectionProfile> = z.object({
  id: z.string(),
  name: z.string(),
  mode: z.enum(["direct", "broker"]),
  base: z.string(),
  brokerBase: z.string(),
  brokerAgentId: z.string(),
  brokerDeploymentId: z.string(),
});

const ClientPrefsConnectionSchema: z.ZodType<ClientPrefsConnection> = z.object({
  active_profile_id: z.string(),
  profiles: z.array(ClientPrefsConnectionProfileSchema),
});

const ClientPrefsPayloadSchema: z.ZodType<ClientPrefsPayload> = z
  .object({
    connection: ClientPrefsConnectionSchema.optional(),
  })
  .catchall(z.unknown());

export const ClientPrefsSchema: z.ZodType<ClientPrefs> = z
  .object({
    ok: z.boolean(),
    found: z.boolean(),
    client_id: z.string(),
    client_kind: z.string(),
    version: z.number().int().optional(),
    updated_utc_ms: z.number().int().nonnegative().optional(),
    prefs: ClientPrefsPayloadSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const ClientPrefsUpdateReqSchema = z
  .object({
    client_id: z.string().min(1),
    client_kind: z.string().default("webui"),
    prefs: ClientPrefsPayloadSchema,
  })
  .passthrough();

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
        provider_keys_set: ProviderKeysSetSchema.optional(),
        auth_enabled: z.boolean().optional(),
        allow_unauthenticated_non_loopback: z.boolean().optional(),
        auth_cookie_name: z.string().optional(),
      })
      .optional(),
    memory: DaemonMemoryConfigSchema.optional(),
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
    memory: DaemonMemoryConfigSchema.optional(),
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
    provider_keys_set: ProviderKeysSetSchema.optional(),
    memory: DaemonMemoryConfigSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
export type DaemonConfigUpdateResp = z.infer<typeof DaemonConfigUpdateRespSchema>;
