import { z } from "zod";

const UnknownValueSchema = z.unknown();

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
  err: z.string().optional(),
  code: z.string().optional(),
});
export type ToolDefsResp = z.infer<typeof ToolDefsRespSchema>;

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
        context_length: UnknownValueSchema.optional(),
        total_usd_per_million: z.number().optional(),
        prompt_usd_per_million: z.number().optional(),
        completion_usd_per_million: z.number().optional(),
        supports_tools: z.boolean().optional(),
        supports_multimodal_input: z.boolean().optional(),
        input_modalities: UnknownValueSchema.optional(),
        output_modalities: UnknownValueSchema.optional(),
      }),
    )
    .optional(),
  error: z.string().optional(),
  err: z.string().optional(),
  code: z.string().optional(),
  http_status: z.number().optional(),
  http_body: z.string().optional(),
});
export type OpenRouterModelsResp = z.infer<typeof OpenRouterModelsRespSchema>;
