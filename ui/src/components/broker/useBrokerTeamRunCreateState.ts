import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";

import {
  apiListRuntimeSkills,
  apiResolveRuntimeSkill,
  type ApiAuth,
  type RuntimeSkillSummary,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import useBrokerTeamRunCreateRequestState from "./useBrokerTeamRunCreateRequestState";
import useBrokerTeamRunRuntimeMembersState from "./useBrokerTeamRunRuntimeMembersState";
import type { TeamMemberRow } from "./types";

type UseBrokerTeamRunCreateStateArgs = {
  base: string;
  daemonBase: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  membersList: TeamMemberRow[];
  teamMeta?: Record<string, unknown> | null;
  onMembersRefresh?: (teamId: string) => Promise<void> | void;
};

type MaterializedTeamRunRequest = {
  run?: Record<string, unknown>;
  team?: Record<string, unknown>;
};

const isObjectRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === "object" && !Array.isArray(value);

const buildRuntimeSkillSeedInputs = (skill: RuntimeSkillSummary | null): Record<string, unknown> => {
  const schema = skill?.inputs_schema;
  const properties = schema?.properties;
  if (!properties || typeof properties !== "object") return {};
  const required = new Set(Array.isArray(schema?.required) ? schema.required : []);
  const out: Record<string, unknown> = {};
  for (const [key, rawProperty] of Object.entries(properties)) {
    if (!required.has(key) && !(isObjectRecord(rawProperty) && "default" in rawProperty)) continue;
    const property = isObjectRecord(rawProperty) ? rawProperty : {};
    const type = typeof property.type === "string" ? property.type : "";
    if ("default" in property) {
      out[key] = property.default;
    } else if (type === "boolean") {
      out[key] = false;
    } else if (type === "integer" || type === "number") {
      out[key] = 0;
    } else {
      out[key] = "";
    }
  }
  return out;
};

export default function useBrokerTeamRunCreateState({
  base,
  daemonBase,
  auth,
  canQuery,
  teamIdTrimmed,
  membersList,
  teamMeta,
  onMembersRefresh,
}: UseBrokerTeamRunCreateStateArgs) {
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runtimeSkillId, setRuntimeSkillId] = useLocalStorageState("agentui.teamRunRuntimeSkillId", "");
  const [runtimeSkillInputsJson, setRuntimeSkillInputsJson] = useLocalStorageState(
    "agentui.teamRunRuntimeSkillInputs",
    "{}",
  );
  const [runtimeSkillError, setRuntimeSkillError] = React.useState<string | null>(null);

  const rolePlanOptions = React.useMemo(() => {
    const teamRoleOverridesDefaults =
      teamMeta?.role_overrides && typeof teamMeta.role_overrides === "object"
        ? (teamMeta.role_overrides as Record<string, unknown>)
        : null;
    const teamRoleOverrideKeys = teamRoleOverridesDefaults
      ? Object.keys(teamRoleOverridesDefaults).map((key) => String(key)).filter(Boolean)
      : [];
    const teamRoleInstructionsDefaults =
      teamMeta?.role_instructions && typeof teamMeta.role_instructions === "object" && !Array.isArray(teamMeta.role_instructions)
        ? (teamMeta.role_instructions as Record<string, unknown>)
        : {};
    const teamRoleInstructionKeys = Object.keys(teamRoleInstructionsDefaults);
    const set = new Set<string>();
    for (const role of teamRoleOverrideKeys) {
      const value = String(role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    for (const role of teamRoleInstructionKeys) {
      const value = String(role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    for (const member of membersList) {
      const value = String(member?.role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    return Array.from(set).filter(Boolean).sort();
  }, [membersList, teamMeta]);

  const runtimeState = useBrokerTeamRunRuntimeMembersState({
    base,
    auth,
    canQuery,
    teamIdTrimmed,
    membersList,
    rolePlanOptions,
    onMembersRefresh,
    setRunError,
  });
  const createRequestState = useBrokerTeamRunCreateRequestState({
    base,
    auth,
    teamIdTrimmed,
    membersList,
    teamMeta,
    runRuntimeMembersJson: runtimeState.runRuntimeMembersJson,
    setRunRuntimeMembersJson: runtimeState.setRunRuntimeMembersJson,
    setRunError,
  });

  const runtimeSkillsQuery = useQuery({
    queryKey: ["runtime_skills", daemonBase, "team_bundle"],
    queryFn: () => apiListRuntimeSkills(daemonBase, { kind: "team_bundle" }, auth),
    enabled: !!String(daemonBase || "").trim(),
    staleTime: 30_000,
  });

  const runtimeSkills = React.useMemo(
    () => runtimeSkillsQuery.data?.skills?.filter((skill) => skill.kind === "team_bundle") ?? [],
    [runtimeSkillsQuery.data?.skills],
  );
  const selectedRuntimeSkill = React.useMemo(
    () => runtimeSkills.find((skill) => skill.skill_id === runtimeSkillId) ?? null,
    [runtimeSkillId, runtimeSkills],
  );
  const runtimeSkillInputsParseError = React.useMemo(() => {
    try {
      const parsed = JSON.parse(runtimeSkillInputsJson || "{}");
      if (!isObjectRecord(parsed)) return "Runtime skill inputs must be a JSON object.";
      return null;
    } catch (err) {
      return `Invalid runtime skill inputs JSON: ${String(err)}`;
    }
  }, [runtimeSkillInputsJson]);

  const selectRuntimeSkill = React.useCallback(
    (skillId: string) => {
      const nextId = String(skillId || "").trim();
      setRuntimeSkillId(nextId);
      const nextSkill = runtimeSkills.find((skill) => skill.skill_id === nextId) ?? null;
      setRuntimeSkillInputsJson(JSON.stringify(buildRuntimeSkillSeedInputs(nextSkill), null, 2));
      setRuntimeSkillError(null);
      setRunError(null);
    },
    [runtimeSkills, setRunError, setRuntimeSkillId, setRuntimeSkillInputsJson],
  );

  const resolveRuntimeSkill = useMutation({
    mutationFn: async (payload: { skillId: string; inputs: Record<string, unknown> }) =>
      apiResolveRuntimeSkill(
        daemonBase,
        {
          skill_id: payload.skillId,
          inputs: payload.inputs,
        },
        auth,
      ),
    onSuccess: (resp) => {
      if (resp.ok === false) {
        setRuntimeSkillError(resp.error || resp.err || resp.code || "Runtime skill resolve failed.");
        return;
      }
      const teamRunRequest = resp.materialized?.team_run_request;
      if (!teamRunRequest || !isObjectRecord(teamRunRequest)) {
        setRuntimeSkillError("Runtime skill did not resolve to a team run payload.");
        return;
      }
      createRequestState.applyMaterializedTeamRunRequest(teamRunRequest as MaterializedTeamRunRequest);
      setRuntimeSkillError(null);
      setRunError(null);
    },
    onError: (err) => {
      setRuntimeSkillError(String(err));
    },
  });

  const applyRuntimeSkill = React.useCallback(async () => {
    const skillId = String(runtimeSkillId || "").trim();
    if (!skillId) {
      setRuntimeSkillError("Select a runtime skill first.");
      return;
    }
    let parsedInputs: unknown = {};
    try {
      parsedInputs = JSON.parse(runtimeSkillInputsJson || "{}");
    } catch (err) {
      setRuntimeSkillError(`Invalid runtime skill inputs JSON: ${String(err)}`);
      return;
    }
    if (!isObjectRecord(parsedInputs)) {
      setRuntimeSkillError("Runtime skill inputs must be a JSON object.");
      return;
    }
    setRuntimeSkillError(null);
    await resolveRuntimeSkill.mutateAsync({ skillId, inputs: parsedInputs });
  }, [resolveRuntimeSkill, runtimeSkillId, runtimeSkillInputsJson]);

  return {
    ...createRequestState,
    ...runtimeState,
    runError,
    setRunError,
    runtimeSkillApplyBusy: resolveRuntimeSkill.isPending,
    runtimeSkillError,
    runtimeSkillInputsJson,
    runtimeSkillInputsParseError,
    runtimeSkillListError:
      runtimeSkillsQuery.error instanceof Error
        ? runtimeSkillsQuery.error.message
        : runtimeSkillsQuery.error
          ? String(runtimeSkillsQuery.error)
          : null,
    runtimeSkillListLoading: runtimeSkillsQuery.isLoading,
    runtimeSkillId,
    runtimeSkills,
    selectedRuntimeSkill,
    applyRuntimeSkill,
    selectRuntimeSkill,
    setRuntimeSkillInputsJson,
  };
}
