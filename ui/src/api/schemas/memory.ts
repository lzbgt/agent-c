import { z } from "zod";
import type { components as AgentdComponents } from "../generated/agentd-openapi";

type ErrorFields = {
  error?: string;
  err?: string;
  code?: string;
};

export type MemoryCheckpointMeta = AgentdComponents["schemas"]["MemoryCheckpointMeta"];
type MemoryQueryResponseBase = AgentdComponents["schemas"]["MemoryQueryResponse"];
type MemoryCorrelateResponseBase = AgentdComponents["schemas"]["MemoryCorrelateResponse"];
type MemoryCorrelationIndexResponseBase = AgentdComponents["schemas"]["MemoryCorrelationIndexBuildResponse"];
type MemoryCheckpointsResponseBase = AgentdComponents["schemas"]["MemoryCheckpointsResponse"];
type MemoryIndexResponseBase = AgentdComponents["schemas"]["MemoryIndexResponse"];
export type MemoryIndexRow = AgentdComponents["schemas"]["MemoryIndexRow"];
export type MemorySaliencePolicy = AgentdComponents["schemas"]["MemorySaliencePolicy"];
export type MemorySalienceStructuredItem = AgentdComponents["schemas"]["MemorySalienceStructuredItem"];
export type MemorySalienceDailyItem = AgentdComponents["schemas"]["MemorySalienceDailyItem"];
type MemorySalienceResponseBase = AgentdComponents["schemas"]["MemorySalienceResponse"];
export type MemoryRecapItem = AgentdComponents["schemas"]["MemoryRecapItem"];
export type MemoryRecapsListItem = AgentdComponents["schemas"]["MemoryRecapsListItem"];
type MemoryRecapsListResponseBase = AgentdComponents["schemas"]["MemoryRecapsListResponse"];
type MemoryRecapResponseBase = AgentdComponents["schemas"]["MemoryRecapResponse"];
export type MemoryCorrelationIndexMeta = AgentdComponents["schemas"]["MemoryCorrelationIndexMeta"];
export type MemoryCorrelateDailyEntry = AgentdComponents["schemas"]["MemoryCorrelateDailyEntry"];
export type MemoryCorrelateRecapEntry = AgentdComponents["schemas"]["MemoryCorrelateRecapEntry"];
export type MemoryCorrelationIndexBuildRequest = AgentdComponents["schemas"]["MemoryCorrelationIndexBuildRequest"];
export type MemoryRetentionEnforceRequest = AgentdComponents["schemas"]["MemoryRetentionEnforceRequest"];
export type MemoryRecapRequest = AgentdComponents["schemas"]["MemoryRecapRequest"];

export type MemoryQueryResp = MemoryQueryResponseBase & ErrorFields;
export type MemoryCorrelateResp = MemoryCorrelateResponseBase & ErrorFields;
export type MemoryCorrelationIndexResp = MemoryCorrelationIndexResponseBase & ErrorFields;
export type MemoryCheckpointsResp = MemoryCheckpointsResponseBase & ErrorFields;
export type MemoryIndexResp = MemoryIndexResponseBase & ErrorFields;
export type MemorySalienceResp = MemorySalienceResponseBase & ErrorFields;
export type MemoryRetentionResp = AgentdComponents["schemas"]["MemoryRetentionEnforceResponse"] & ErrorFields;
export type MemoryRecapsListResp = MemoryRecapsListResponseBase & ErrorFields;
export type MemoryRecapResp = MemoryRecapResponseBase & ErrorFields;

const UnknownRecordSchema = z.record(z.string(), z.unknown());

const MemoryCheckpointMetaSchema: z.ZodType<MemoryCheckpointMeta> = z
  .object({
    checkpoint_path: z.string(),
    structured_path: z.string().optional(),
    ts_utc: z.string(),
    ts_utc_ms: z.number().int().nonnegative(),
    sha256: z.string(),
    bytes: z.number().int().nonnegative(),
  })
  .passthrough();

const MemoryQueryEntrySchema = z
  .object({
    key: z.string(),
    record: UnknownRecordSchema,
  })
  .passthrough();

const MemoryCorrelationIndexMetaSchema: z.ZodType<MemoryCorrelationIndexMeta> = z
  .object({
    ok: z.boolean().optional(),
    token: z.string().optional(),
    index_path: z.string().optional(),
    generated_utc_ms: z.number().int().nonnegative().optional(),
    generated_utc: z.string().optional(),
    token_count: z.number().int().nonnegative().optional(),
    entry_count: z.number().int().nonnegative().optional(),
    structured_entries: z.number().int().nonnegative().optional(),
    daily_entries: z.number().int().nonnegative().optional(),
    recap_entries: z.number().int().nonnegative().optional(),
    error: z.string().optional(),
  })
  .passthrough();

const MemoryCorrelateDailyEntrySchema: z.ZodType<MemoryCorrelateDailyEntry> = z
  .object({
    path: z.string().optional(),
    line: z.number().int().nonnegative().optional(),
    text: z.string().optional(),
    trace_id: z.string().optional(),
    source: z.string().optional(),
    ts_utc: z.string().optional(),
    importance: z.number().int().nonnegative().optional(),
  })
  .passthrough();

const MemoryCorrelateRecapEntrySchema: z.ZodType<MemoryCorrelateRecapEntry> = z
  .object({
    recap_path: z.string().optional(),
    kind: z.string().optional(),
    ts_utc: z.string().optional(),
    ts_utc_ms: z.number().int().nonnegative().optional(),
    summary_excerpt: z.string().optional(),
    evidence_sources: z.array(z.string()).optional(),
  })
  .passthrough();

const MemoryCorrelateTimelineEntrySchema = z
  .object({
    checkpoint: MemoryCheckpointMetaSchema,
    entries: z.array(MemoryQueryEntrySchema),
  })
  .passthrough();

const MemoryIndexRowSchema: z.ZodType<MemoryIndexRow> = z
  .object({
    tier: z.string(),
    path: z.string(),
    bytes: z.number().int().nonnegative(),
    lines: z.number().int().nonnegative(),
    token_estimate: z.number().int().nonnegative(),
  })
  .passthrough();

const MemorySaliencePolicySchema: z.ZodType<MemorySaliencePolicy> = z
  .object({
    include_structured: z.boolean(),
    include_daily: z.boolean(),
    daily_days: z.number().int().nonnegative(),
    max_items: z.number().int().nonnegative(),
    max_structured_items: z.number().int().nonnegative(),
    max_daily_items: z.number().int().nonnegative(),
    half_life_days: z.number().nonnegative(),
    importance_weight: z.number().nonnegative(),
  })
  .passthrough();

const MemorySalienceStructuredItemSchema: z.ZodType<MemorySalienceStructuredItem> = z
  .object({
    key: z.string(),
    kind: z.string().optional(),
    status: z.string().optional(),
    updated_utc: z.string().optional(),
    value: z.string(),
    score: z.number(),
  })
  .passthrough();

const MemorySalienceDailyItemSchema: z.ZodType<MemorySalienceDailyItem> = z
  .object({
    path: z.string(),
    line: z.number().int().nonnegative(),
    text: z.string(),
    score: z.number(),
    ts_utc: z.string().optional(),
    importance: z.number().int().nonnegative().optional(),
  })
  .passthrough();

const MemoryRecapItemSchema: z.ZodType<MemoryRecapItem> = z
  .object({
    tier: z.string(),
    key: z.string().optional(),
    kind: z.string().optional(),
    status: z.string().optional(),
    ts_utc: z.string().optional(),
    path: z.string().optional(),
    line: z.number().int().nonnegative().optional(),
    importance: z.number().int().nonnegative().optional(),
    score: z.number(),
    text: z.string(),
    source: z.string().optional(),
  })
  .passthrough();

const MemoryRecapsListItemSchema: z.ZodType<MemoryRecapsListItem> = z
  .object({
    recap_path: z.string(),
    bytes: z.number().int().nonnegative(),
    ts_utc: z.string().optional(),
    ts_utc_ms: z.number().int().nonnegative().optional(),
    model: z.string().optional(),
    kind: z.string().optional(),
    summary_text: z.string().optional(),
  })
  .passthrough();

export const MemoryQueryRespSchema: z.ZodType<MemoryQueryResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    since_utc_ms: z.number().int().nonnegative(),
    until_utc_ms: z.number().int().nonnegative(),
    structured_path_filter: z.string().optional(),
    key_prefix: z.string().optional(),
    limit: z.number().int().nonnegative(),
    returned: z.number().int().nonnegative(),
    checkpoint: MemoryCheckpointMetaSchema,
    entries: z.array(MemoryQueryEntrySchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryCorrelateRespSchema: z.ZodType<MemoryCorrelateResp> = z
  .object({
    ok: z.boolean(),
    trace_id: z.string(),
    needle: z.string(),
    since_utc_ms: z.number().int().nonnegative(),
    until_utc_ms: z.number().int().nonnegative(),
    structured_path_filter: z.string().optional(),
    key_prefix: z.string().optional(),
    checkpoint: MemoryCheckpointMetaSchema.optional(),
    entries: z.array(MemoryQueryEntrySchema).optional(),
    index: MemoryCorrelationIndexMetaSchema.optional(),
    daily_entries: z.array(MemoryCorrelateDailyEntrySchema).optional(),
    recap_entries: z.array(MemoryCorrelateRecapEntrySchema).optional(),
    timeline: z.array(MemoryCorrelateTimelineEntrySchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryCorrelationIndexRespSchema: z.ZodType<MemoryCorrelationIndexResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    generated_utc_ms: z.number().int().nonnegative(),
    generated_utc: z.string().optional(),
    index_path: z.string(),
    token_count: z.number().int().nonnegative(),
    entry_count: z.number().int().nonnegative(),
    structured_entries: z.number().int().nonnegative(),
    daily_entries: z.number().int().nonnegative(),
    recap_entries: z.number().int().nonnegative(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryCheckpointsRespSchema: z.ZodType<MemoryCheckpointsResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    since_utc_ms: z.number().int().nonnegative(),
    until_utc_ms: z.number().int().nonnegative(),
    structured_path_filter: z.string().optional(),
    checkpoints: z.array(MemoryCheckpointMetaSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryIndexRespSchema: z.ZodType<MemoryIndexResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    session_id: z.string().optional(),
    daily_days: z.number().int().nonnegative(),
    include_structured: z.boolean(),
    include_core: z.boolean(),
    include_daily: z.boolean(),
    include_session: z.boolean(),
    total_bytes: z.number().int().nonnegative().optional(),
    total_token_estimate: z.number().int().nonnegative().optional(),
    files: z.array(MemoryIndexRowSchema),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemorySalienceRespSchema: z.ZodType<MemorySalienceResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    generated_utc_ms: z.number().int().nonnegative(),
    policy: MemorySaliencePolicySchema,
    structured_checkpoint: MemoryCheckpointMetaSchema.optional(),
    structured_items: z.array(MemorySalienceStructuredItemSchema),
    daily_items: z.array(MemorySalienceDailyItemSchema),
    returned: z.number().int().nonnegative(),
    errors: z.array(z.string()).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryRetentionRespSchema: z.ZodType<MemoryRetentionResp> = z
  .object({
    ok: z.boolean(),
    generated_utc_ms: z.number().int().nonnegative(),
    dry_run: z.boolean(),
    daily_max_days: z.number().int().nonnegative(),
    daily_max_bytes: z.number().int().nonnegative(),
    checkpoint_max_days: z.number().int().nonnegative(),
    checkpoint_max_count: z.number().int().nonnegative(),
    structured_deprecate_days: z.number().int().nonnegative(),
    structured_deprecate_max_entries: z.number().int().nonnegative(),
    daily_deleted_count: z.number().int().nonnegative().optional(),
    checkpoint_deleted_count: z.number().int().nonnegative().optional(),
    structured_deprecated_count: z.number().int().nonnegative().optional(),
    daily_bytes_before: z.number().int().nonnegative().optional(),
    daily_bytes_after: z.number().int().nonnegative().optional(),
    daily_deleted: z.array(z.string()).optional(),
    checkpoint_deleted: z.array(z.string()).optional(),
    structured_deprecated_keys: z.array(z.string()).optional(),
    errors: z.array(z.string()).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryRecapsListRespSchema: z.ZodType<MemoryRecapsListResp> = z
  .object({
    ok: z.boolean(),
    memory_root: z.string(),
    limit: z.number().int().nonnegative(),
    include_summary: z.boolean(),
    recaps: z.array(MemoryRecapsListItemSchema),
    count: z.number().int().nonnegative(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const MemoryRecapRespSchema: z.ZodType<MemoryRecapResp> = z
  .object({
    ok: z.boolean(),
    generated_utc_ms: z.number().int().nonnegative(),
    ts_utc: z.string().optional(),
    model: z.string().optional(),
    kind: z.string().optional(),
    dry_run: z.boolean(),
    write_file: z.boolean(),
    policy: MemorySaliencePolicySchema,
    input: z
      .object({
        structured_count: z.number().int().nonnegative(),
        daily_count: z.number().int().nonnegative(),
        total_count: z.number().int().nonnegative(),
      })
      .passthrough(),
    structured_items: z.array(MemoryRecapItemSchema),
    daily_items: z.array(MemoryRecapItemSchema),
    evidence_sources: z.array(z.string()).optional(),
    prompt: z.string().optional(),
    prompt_truncated: z.boolean().optional(),
    summary_text: z.string().optional(),
    summary: UnknownRecordSchema.optional(),
    recap_path: z.string().optional(),
    recap_bytes: z.number().int().nonnegative().optional(),
    correlation_index: MemoryCorrelationIndexMetaSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();
