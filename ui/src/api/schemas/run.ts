import { z } from "zod";

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
  automation_profile: z.enum(["full", "guided", "strict", "custom"]).optional(),
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
      }),
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
  memory_search_order: z.enum(["ranked", "newest", "oldest"]).optional(),
  memory_search_fallback_to_files: z.boolean().optional(),
  memory_search_max_results: z.number().int().nonnegative().optional(),
  memory_search_max_snippet_chars: z.number().int().nonnegative().optional(),
  memory_search_context_lines: z.number().int().nonnegative().optional(),
  trace: z.boolean().optional(),
  yolo: z.boolean().optional(),
  verbose: z.boolean().optional(),
});
export type RunRequest = z.infer<typeof RunRequestSchema>;

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
  err: z.string().optional(),
  code: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
  trace_text: z.string().optional(),
  effective_yolo: z.boolean().optional(),
  effective_host_policy: z.enum(["full", "readonly"]).optional(),
  effective_automation_profile: z.enum(["full", "guided", "strict", "custom"]).optional(),
  effective_timeout_ms: z.number().optional(),
  effective_max_tool_call_args_chars: z.number().optional(),
  effective_stream_assistant: z.boolean().optional(),
  verbose: z.boolean().optional(),
  events: z.array(EventSchema).optional(),
});
export type RunResponse = z.infer<typeof RunResponseSchema>;

export const RunAsyncRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  trace_id: z.string().optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type RunAsyncResp = z.infer<typeof RunAsyncRespSchema>;

export const JobRespSchema = z.object({
  ok: z.boolean(),
  job_id: z.string().optional(),
  trace_id: z.string().optional(),
  status: z.string().optional(), // queued|running|done|error|cancelled
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
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

export const RunReplayBundleSchema = z.object({
  schema: z.string().optional(),
  request: z.any().optional(),
  response: z.any().optional(),
  tool_records: z.array(z.any()).optional(),
});
export type RunReplayBundle = z.infer<typeof RunReplayBundleSchema>;

export const RunReplayRespSchema = z.object({
  ok: z.boolean(),
  run_id: z.union([z.number(), z.string()]).optional(),
  session_id: z.string().optional(),
  bundle: RunReplayBundleSchema.optional(),
  replay_sha256: z.string().optional(),
  replay_sha256_alg: z.string().optional(),
  replay_sha256_schema: z.string().optional(),
  replay_error: z.string().optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type RunReplayResp = z.infer<typeof RunReplayRespSchema>;

export const RunAttestationBundleSchema = z
  .object({
    schema: z.string().optional(),
    created_utc_ms: z.number().optional(),
    replay_sha256: z.string().optional(),
    replay_sha256_alg: z.string().optional(),
    replay_sha256_schema: z.string().optional(),
    run_id: z.union([z.number(), z.string()]).optional(),
    session_id: z.string().optional(),
    agent_id: z.string().optional(),
    deployment_id: z.string().optional(),
    issuer: z.any().optional(),
    attest: z.any().optional(),
  })
  .passthrough();
export type RunAttestationBundle = z.infer<typeof RunAttestationBundleSchema>;

export const RunAttestationRespSchema = z.object({
  ok: z.boolean(),
  run_id: z.union([z.number(), z.string()]).optional(),
  session_id: z.string().optional(),
  attestation: RunAttestationBundleSchema.optional(),
  replay_error: z.string().optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
});
export type RunAttestationResp = z.infer<typeof RunAttestationRespSchema>;
