import React from "react";
import type { ApiAuth } from "../../api";
import type { TeamMemberRow } from "./types";
import useBrokerTeamRunCreateRequestState from "./useBrokerTeamRunCreateRequestState";
import useBrokerTeamRunRuntimeMembersState from "./useBrokerTeamRunRuntimeMembersState";

type UseBrokerTeamRunCreateStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  membersList: TeamMemberRow[];
  teamMeta?: Record<string, unknown> | null;
  onMembersRefresh?: (teamId: string) => Promise<void> | void;
};

export default function useBrokerTeamRunCreateState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  membersList,
  teamMeta,
  onMembersRefresh,
}: UseBrokerTeamRunCreateStateArgs) {
  const [runError, setRunError] = React.useState<string | null>(null);
  const runtimeState = useBrokerTeamRunRuntimeMembersState({
    base,
    auth,
    canQuery,
    teamIdTrimmed,
    membersList,
    rolePlanOptions:
      (() => {
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
      })(),
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
    setRunError,
  });

  return {
    ...createRequestState,
    ...runtimeState,
    runError,
    setRunError,
  };
}
