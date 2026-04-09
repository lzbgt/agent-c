import type { BrokerTeamRunApproval, BrokerTeamRunRuntimeMember } from "../../api";
import type { TeamMemberRow } from "./types";

export type RuntimeMemberDraft = Partial<BrokerTeamRunRuntimeMember> & {
  meta?: Record<string, unknown>;
};

export type RuntimePreviewRow = {
  item: RuntimeMemberDraft;
  reason: string;
};

export type RuntimeTeamDiffRow = {
  item: RuntimeMemberDraft;
  team: TeamMemberRow;
  diffs: string[];
};

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

export type TeamRunApprovalRow = BrokerTeamRunApproval;

export type RuntimeMembersPreview = {
  items: RuntimeMemberDraft[];
  error: string;
};

export type RuntimeSavePreview = {
  newMembers: RuntimePreviewRow[];
  skipped: RuntimePreviewRow[];
  invalid: RuntimePreviewRow[];
};

export type RuntimeTeamDiff = {
  runtimeOnly: RuntimeMemberDraft[];
  teamOnly: TeamMemberRow[];
  mismatched: RuntimeTeamDiffRow[];
};
