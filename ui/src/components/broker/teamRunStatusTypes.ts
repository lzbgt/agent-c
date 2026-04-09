import type {
  BrokerTeamRunCancelResult,
  BrokerTeamRunDispatchError,
  BrokerTeamRunGoalContract,
  BrokerTeamRunGoalEvent,
  BrokerTeamRunHandoffEvent,
  BrokerTeamRunMemberJob,
  BrokerTeamRunMemberJobSummary,
  BrokerTeamRunModeratorDispatch,
  BrokerTeamRunModeratorEvent,
  BrokerTeamRunModeratorSkipped,
  BrokerTeamRunResp,
  BrokerTeamRunRuntimeMember,
  BrokerTeamRunStatusResp,
  BrokerTeamRunSummary,
} from "../../api";

export type MemberSession = {
  memberId: string;
  sessionId: string;
};

export type TeamRunLookupResult = BrokerTeamRunStatusResp;
export type TeamRunCreateResult = BrokerTeamRunResp;
export type TeamRunRecentRun = BrokerTeamRunSummary;
export type TeamRunGoalContractRow = BrokerTeamRunGoalContract;
export type TeamRunGoalEventRow = BrokerTeamRunGoalEvent;
export type TeamRunGoalEventType = "progress" | "drift" | "spawn_validation";
export type TeamRunGoalEventInput = Pick<TeamRunGoalEventRow, "type"> & {
  message?: string;
  ts_unix_ms?: number;
  data?: Record<string, unknown>;
};
export type TeamRunHandoffKind = "role" | "cross_deployment";
export type TeamRunHandoffState = "proposed" | "accepted" | "declined" | "cancelled";
export type TeamRunHandoffTransitionState = Exclude<TeamRunHandoffState, "proposed">;
export type TeamRunHandoffEventRow = BrokerTeamRunHandoffEvent;
export type TeamRunHandoffEventRecord = Partial<Omit<TeamRunHandoffEventRow, "data" | "kind" | "state">> & {
  kind?: TeamRunHandoffKind;
  state?: TeamRunHandoffState;
  data?: Record<string, unknown>;
};
export type TeamRunHandoffEventInput = Partial<Omit<TeamRunHandoffEventRow, "data" | "kind" | "state">> & {
  kind?: TeamRunHandoffKind;
  state?: TeamRunHandoffState;
  data?: Record<string, unknown>;
};
export type TeamRunRuntimeMemberRow = BrokerTeamRunRuntimeMember;
export type TeamRunMemberJobRow = BrokerTeamRunMemberJob;
export type TeamRunDispatchErrorRow = BrokerTeamRunDispatchError;
export type TeamRunCancelResultRow = BrokerTeamRunCancelResult;
export type TeamRunMemberJobSummaryRow = BrokerTeamRunMemberJobSummary;
export type TeamRunModeratorEventRow = BrokerTeamRunModeratorEvent;
export type TeamRunModeratorDispatchRow = BrokerTeamRunModeratorDispatch;
export type TeamRunModeratorSkippedRow = BrokerTeamRunModeratorSkipped;
