import { daemonFetchInit, type ApiAuth } from "./auth";
import { addQueryParam } from "./query";
import {
  RuntimeSkillListRespSchema,
  RuntimeSkillResolveRespSchema,
  type RuntimeSkillListResp,
  type RuntimeSkillResolveResp,
} from "./schemas/runtime_skills";

export type RuntimeSkillListParams = {
  kind?: string;
  category?: string;
};

export async function apiListRuntimeSkills(
  base: string,
  params: RuntimeSkillListParams,
  auth?: ApiAuth,
): Promise<RuntimeSkillListResp> {
  const qs = new URLSearchParams();
  addQueryParam(qs, "kind", params.kind);
  addQueryParam(qs, "category", params.category);
  const q = qs.toString();
  const r = await fetch(`${base}/api/v1/runtime_skills${q ? `?${q}` : ""}`, daemonFetchInit(auth));
  const j = await r.json();
  return RuntimeSkillListRespSchema.parse(j);
}

export type RuntimeSkillResolveRequest = {
  skill_id: string;
  inputs?: Record<string, unknown>;
};

export async function apiResolveRuntimeSkill(
  base: string,
  payload: RuntimeSkillResolveRequest,
  auth?: ApiAuth,
): Promise<RuntimeSkillResolveResp> {
  const r = await fetch(
    `${base}/api/v1/runtime_skills/resolve`,
    daemonFetchInit(auth, { method: "POST", body: JSON.stringify(payload) }, { "Content-Type": "application/json" }),
  );
  const j = await r.json();
  return RuntimeSkillResolveRespSchema.parse(j);
}
