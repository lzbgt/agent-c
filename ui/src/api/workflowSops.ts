import { z } from "zod";
import { daemonFetchInit, type ApiAuth } from "./auth";
import type { components } from "./generated/agentd-openapi";

export type WorkflowSopReference = components["schemas"]["WorkflowSopReference"];
const DefinitionSchema = z.object({
  registry_id: z.number().int().positive(),
  id: z.string(),
  version: z.string(),
  definition_hash: z.string(),
  title: z.string(),
  created_unix_ms: z.number(),
});
export type WorkflowSopDefinition = z.infer<typeof DefinitionSchema>;
const PageSchema = z.object({
  definitions: z.array(DefinitionSchema),
  next_before_id: z.number().int().positive().optional(),
});
const SaveSchema = z.object({ created: z.boolean(), definition: DefinitionSchema });

async function responseJson(response: Response) {
  const body = await response.json();
  if (!response.ok || body?.ok !== true) {
    throw new Error(typeof body?.error === "string" ? body.error : "Saved SOP library is unavailable on this connection.");
  }
  return body;
}

export async function listWorkflowSops(base: string, auth?: ApiAuth, beforeId?: number, signal?: AbortSignal) {
  const query = new URLSearchParams({ limit: "10" });
  if (beforeId) query.set("before_id", String(beforeId));
  const response = await fetch(`${base}/api/v1/workflow/sops?${query}`, daemonFetchInit(auth, { signal }));
  return PageSchema.parse(await responseJson(response));
}

export async function saveWorkflowSop(base: string, sop: unknown, auth?: ApiAuth, signal?: AbortSignal) {
  const response = await fetch(`${base}/api/v1/workflow/sops`, daemonFetchInit(auth,
    { method: "POST", body: JSON.stringify({ sop }), signal }, { "Content-Type": "application/json" }));
  return SaveSchema.parse(await responseJson(response));
}
