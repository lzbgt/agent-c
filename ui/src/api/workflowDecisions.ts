import { z } from "zod";
import { daemonFetchInit, type ApiAuth } from "./auth";
import { WorkflowDecisionSchema } from "./schemas/workflow";
import type { components } from "./generated/agentd-openapi";

export type DecisionRequest = components["schemas"]["WorkflowDecisionRequest"];
const ReceiptSchema = z.object({ ok: z.literal(true), replayed: z.boolean(), decision: WorkflowDecisionSchema });

export async function recordWorkflowDecision(base: string, request: DecisionRequest, auth?: ApiAuth, signal?: AbortSignal) {
  const response = await fetch(`${base}/api/v1/workflow/decision`, daemonFetchInit(auth,
    { method: "POST", body: JSON.stringify(request), signal }, { "Content-Type": "application/json" }));
  const body = await response.json();
  if (!response.ok || body?.ok !== true) throw new Error(typeof body?.error === "string" ? body.error : "Decision receipt was not confirmed.");
  return ReceiptSchema.parse(body);
}
