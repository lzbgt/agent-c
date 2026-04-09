import React from "react";
import type { ApiAuth } from "../../api";
import type { TeamRunCreateResult } from "./teamRunStatusTypes";
import type { BrokerEventRow } from "./types";
import useBrokerTeamRunApprovalsState from "./useBrokerTeamRunApprovalsState";
import useBrokerTeamRunLookupState from "./useBrokerTeamRunLookupState";
import useBrokerTeamRunModeratorState from "./useBrokerTeamRunModeratorState";

type UseBrokerTeamRunControlStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  quorumEvents?: BrokerEventRow[];
  runResult: TeamRunCreateResult | null;
  runRuntimeMembersJson: string;
  setRunRuntimeMembersJson: (value: string) => void;
};

export default function useBrokerTeamRunControlState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  quorumEvents,
  runResult,
  runRuntimeMembersJson,
  setRunRuntimeMembersJson,
}: UseBrokerTeamRunControlStateArgs) {
  const lookupState = useBrokerTeamRunLookupState({
    base,
    auth,
    canQuery,
    teamIdTrimmed,
    quorumEvents,
    runResult,
    runRuntimeMembersJson,
    setRunRuntimeMembersJson,
  });
  const moderatorState = useBrokerTeamRunModeratorState({
    base,
    auth,
    teamIdTrimmed,
    resolveRunId: lookupState.resolveRunId,
  });
  const approvalsState = useBrokerTeamRunApprovalsState({
    base,
    auth,
    canQuery,
    teamIdTrimmed,
    runLookupId: lookupState.runLookupId,
    runLookupResult: lookupState.runLookupResult,
    runResult,
  });

  return {
    ...lookupState,
    ...moderatorState,
    ...approvalsState,
  };
}
