export type BrokerEventRow = {
  type: string;
  ts_unix_ms?: number;
  event_id?: string;
  trace_id?: string;
  payload?: Record<string, any>;
};

export type TeamMemberRow = {
  member_id?: string;
  role?: string;
  status?: string;
  agent_id?: string;
  deployment_id?: string;
  created_unix_ms?: number;
  weight?: number;
  capabilities?: string[];
  meta?: Record<string, any>;
};

export type TeamQuorumRuleRow = {
  rule_id?: string;
  action?: string;
  min_approvals?: number;
  quorum_mode?: string;
  created_unix_ms?: number;
};
