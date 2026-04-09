import type { BrokerTeamMember, BrokerTeamQuorumRule } from "../../api";

export type BrokerEventRow = {
  type: string;
  ts_unix_ms?: number;
  event_id?: string;
  trace_id?: string;
  payload?: Record<string, unknown>;
};

export type TeamMemberRow = BrokerTeamMember;

export type TeamQuorumRuleRow = BrokerTeamQuorumRule;

export type TeamCursorEntry = {
  cursor_ts?: number;
  updated_unix_ms?: number;
};
