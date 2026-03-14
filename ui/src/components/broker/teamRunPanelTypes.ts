import type { TeamMemberRow } from "./types";

export type QuorumRuleEval = {
  rule_id?: string;
  action?: string;
  quorum_mode?: string;
  min_approvals?: number;
  approved?: number;
  missing?: number;
  ok?: boolean;
  role_allowlist?: string[];
  require_distinct_roles?: boolean;
  approved_member_ids?: string[];
  approved_roles?: string[];
};

export type QuorumEval = {
  strict_ok?: boolean;
  rules?: QuorumRuleEval[];
};

export type TeamRunApprovalRow = {
  approval_id?: string;
  team_id?: string;
  team_run_id?: string;
  rule_id?: string;
  member_id?: string;
  role?: string;
  decision?: string;
  reason?: string;
  created_by?: string;
  created_unix_ms?: number;
};

export type RuntimeMembersPreview = {
  items: any[];
  error: string;
};

export type RuntimeSavePreview = {
  newMembers: any[];
  skipped: any[];
  invalid: any[];
};

export type RuntimeTeamDiff = {
  runtimeOnly: any[];
  teamOnly: TeamMemberRow[];
  mismatched: any[];
};
