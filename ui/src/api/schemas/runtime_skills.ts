import { z } from "zod";

const RuntimeSkillRequiresSchema = z
  .object({
    tools: z.array(z.string()).optional(),
    plugins: z.array(z.string()).optional(),
    features: z.array(z.string()).optional(),
  })
  .passthrough();

export const RuntimeSkillInputPropertySchema = z
  .object({
    type: z.string().optional(),
    description: z.string().optional(),
    enum: z.array(z.union([z.string(), z.number(), z.boolean(), z.null()])).optional(),
  })
  .passthrough();

export const RuntimeSkillInputsSchemaSchema = z
  .object({
    type: z.string().optional(),
    properties: z.record(z.string(), RuntimeSkillInputPropertySchema).optional(),
    required: z.array(z.string()).optional(),
    additionalProperties: z.unknown().optional(),
  })
  .passthrough();

const RuntimeSkillUiSchema = z
  .object({
    label: z.string().optional(),
    category: z.string().optional(),
    icon: z.string().optional(),
  })
  .passthrough();

export const RuntimeSkillSummarySchema = z
  .object({
    skill_id: z.string(),
    version: z.string(),
    kind: z.string(),
    description: z.string(),
    category: z.string().optional(),
    label: z.string().optional(),
    source_manifest: z.string().optional(),
    root: z.string().optional(),
    requires: RuntimeSkillRequiresSchema.optional(),
    inputs_schema: RuntimeSkillInputsSchemaSchema.nullable().optional(),
    ui: RuntimeSkillUiSchema.optional(),
    has_workflow_template: z.boolean().optional(),
    has_team_template: z.boolean().optional(),
    has_policy_preset: z.boolean().optional(),
  })
  .passthrough();

export const RuntimeSkillCapabilitiesSchema = z
  .object({
    tools: z.array(z.string()).optional(),
    plugins: z.array(z.string()).optional(),
    features: z.array(z.string()).optional(),
  })
  .passthrough();

export const RuntimeSkillListRespSchema = z
  .object({
    ok: z.boolean(),
    capabilities: RuntimeSkillCapabilitiesSchema.optional(),
    skills: z.array(RuntimeSkillSummarySchema).optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
  })
  .passthrough();

export const RuntimeSkillResolveRespSchema = z
  .object({
    ok: z.boolean().optional(),
    skill_id: z.string().optional(),
    skill_version: z.string().optional(),
    description: z.string().optional(),
    kind: z.string().optional(),
    manifest_sha256: z.string().optional(),
    source_manifest: z.string().optional(),
    inputs: z.record(z.string(), z.unknown()).optional(),
    manifest: z.record(z.string(), z.unknown()).optional(),
    resolved: z.record(z.string(), z.unknown()).optional(),
    materialized: z
      .object({
        workflow_request: z.record(z.string(), z.unknown()).optional(),
      })
      .passthrough()
      .optional(),
    capabilities_checked: z.boolean().optional(),
    capabilities: RuntimeSkillCapabilitiesSchema.optional(),
    error: z.string().optional(),
    err: z.string().optional(),
    code: z.string().optional(),
    details: z.record(z.string(), z.unknown()).optional(),
  })
  .passthrough();

export type RuntimeSkillSummary = z.infer<typeof RuntimeSkillSummarySchema>;
export type RuntimeSkillInputsSchema = z.infer<typeof RuntimeSkillInputsSchemaSchema>;
export type RuntimeSkillInputProperty = z.infer<typeof RuntimeSkillInputPropertySchema>;
export type RuntimeSkillCapabilities = z.infer<typeof RuntimeSkillCapabilitiesSchema>;
export type RuntimeSkillListResp = z.infer<typeof RuntimeSkillListRespSchema>;
export type RuntimeSkillResolveResp = z.infer<typeof RuntimeSkillResolveRespSchema>;
