import type { ApiAuth, ApprovalDecision, ApprovalRequest } from "../../api";

export type ApprovalQueuePanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

export type ApprovalQueueState = {
  approvals: ApprovalRequest[];
  auth: ApiAuth;
  base: string;
  canQuery: boolean;
  clearApprovals: () => void;
  decisionBusy: boolean;
  decisionError: string | null;
  detailApproval: ApprovalRequest | null;
  detailBusy: boolean;
  detailDecisions: ApprovalDecision[];
  detailError: string | null;
  isPending: boolean;
  jobIdFilter: string;
  limitFilter: string;
  listBusy: boolean;
  listError: string | null;
  loadApprovals: () => Promise<void>;
  memberId: string;
  memberRole: string;
  note: string;
  runIdFilter: string;
  selectedId: string | null;
  selectApproval: (approvalId: string) => void;
  setJobIdFilter: (next: string) => void;
  setLimitFilter: (next: string) => void;
  setMemberId: (next: string) => void;
  setMemberRole: (next: string) => void;
  setNote: (next: string) => void;
  setRunIdFilter: (next: string) => void;
  setStatusFilter: (next: string) => void;
  setTeamIdFilter: (next: string) => void;
  setToolNameFilter: (next: string) => void;
  setTraceIdFilter: (next: string) => void;
  statusFilter: string;
  submitDecision: (decision: "approve" | "deny") => Promise<void>;
  teamIdFilter: string;
  toolNameFilter: string;
  traceIdFilter: string;
};
