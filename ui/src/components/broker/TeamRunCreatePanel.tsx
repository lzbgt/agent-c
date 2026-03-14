import React from "react";
import FieldLabel from "../FieldLabel";
import TeamRunCreateAdvancedSection from "./TeamRunCreateAdvancedSection";
import TeamRunCreateApprovalsSection from "./TeamRunCreateApprovalsSection";
import TeamRunCreateRuntimeSection from "./TeamRunCreateRuntimeSection";

export type InlineApproval = {
  member_id: string;
  decision: "approve" | "deny";
  rule_id?: string;
  reason?: string;
};

export type TeamRunCreatePanelProps = {
  canQuery: boolean;
  teamId: string;
  runPrompt: string;
  setRunPrompt: (value: string) => void;
  runModel: string;
  setRunModel: (value: string) => void;
  runTools: string;
  setRunTools: (value: string) => void;
  runRole: string;
  setRunRole: (value: string) => void;
  runRoles: string;
  setRunRoles: (value: string) => void;
  runConcurrency: string;
  setRunConcurrency: (value: string) => void;
  runTimeoutMs: string;
  setRunTimeoutMs: (value: string) => void;
  runSharedMemoryScope: string;
  setRunSharedMemoryScope: (value: string) => void;
  runSharedMemoryMode: string;
  setRunSharedMemoryMode: (value: string) => void;
  runAutoAllocateRoles: boolean;
  setRunAutoAllocateRoles: (value: boolean) => void;
  runAutoAllocateMaxMembers: string;
  setRunAutoAllocateMaxMembers: (value: string) => void;
  runMode: string;
  setRunMode: (value: string) => void;
  runQuorumMode: string;
  setRunQuorumMode: (value: string) => void;
  runBusy: boolean;
  onCreateRun: () => void;
  runOverridesMode: string;
  setRunOverridesMode: (value: string) => void;
  runMemberOverridesJson: string;
  setRunMemberOverridesJson: (value: string) => void;
  onSeedExplicitOverrides: () => void;
  runRoleOverridesJson: string;
  setRunRoleOverridesJson: (value: string) => void;
  teamRoleOverrideKeys: string[];
  onSeedRoleOverrides: () => void;
  runRoleInstructionsOverride: boolean;
  setRunRoleInstructionsOverride: (value: boolean) => void;
  runRoleInstructions: Record<string, string>;
  setRunRoleInstructions: (next: Record<string, string>) => void;
  runRolePromptMode: string;
  setRunRolePromptMode: (value: string) => void;
  teamRoleInstructionKeys: string[];
  onSeedRoleInstructions: () => void;
  runRolePlanOptions: string[];
  runRuntimeMembersJson: string;
  setRunRuntimeMembersJson: (value: string) => void;
  runtimeMembersPreview: { items: any[]; error: string };
  runtimeTeamDiff: { runtimeOnly: any[]; teamOnly: any[]; mismatched: any[] };
  handleSetAllRuntimeStatus: (status: "active" | "paused") => void;
  handleRemovePausedRuntimeMembers: () => void;
  handleCompactRuntimeMembers: () => void;
  handleCopyRuntimeMembers: () => void;
  runtimeImportRef: React.RefObject<HTMLInputElement>;
  runtimeImportMerge: boolean;
  setRuntimeImportMerge: (value: boolean) => void;
  handleDownloadRuntimeMembers: () => void;
  handleExportTeamMembers: () => void;
  handleImportRuntimeMembers: (event: React.ChangeEvent<HTMLInputElement>) => void;
  handleToggleRuntimeMemberStatus: (idx: number) => void;
  handleRemoveRuntimeMember: (idx: number) => void;
  runtimeMemberId: string;
  setRuntimeMemberId: (value: string) => void;
  runtimeMemberAgentId: string;
  setRuntimeMemberAgentId: (value: string) => void;
  runtimeMemberRole: string;
  setRuntimeMemberRole: (value: string) => void;
  runtimeAgentOptions: any[];
  runtimeAgentsBusy: boolean;
  refreshRuntimeAgents: () => void;
  handleAddConnectedAgents: () => void;
  handleAllocateRoleRuntimeMembers: () => void;
  runtimeSaveBusy: boolean;
  handleSaveRuntimeMembers: () => void;
  runtimeAgentsError: string | null;
  runtimeSaveError: string | null;
  runtimeSavePreview: { newMembers: any[]; skipped: any[]; invalid: any[] };
  handleFixInvalidRuntimeMembers: () => void;
  runtimeMemberDeploymentId: string;
  setRuntimeMemberDeploymentId: (value: string) => void;
  runtimeAgentDeployments: any[];
  runtimeMemberCapabilities: string;
  setRuntimeMemberCapabilities: (value: string) => void;
  runtimeMemberBackendLabel: string;
  setRuntimeMemberBackendLabel: (value: string) => void;
  runtimeMemberModel: string;
  setRuntimeMemberModel: (value: string) => void;
  runtimeMemberBaseUrl: string;
  setRuntimeMemberBaseUrl: (value: string) => void;
  runtimeMemberSummaryModel: string;
  setRuntimeMemberSummaryModel: (value: string) => void;
  runtimeMemberTools: string;
  setRuntimeMemberTools: (value: string) => void;
  runtimeMemberTimeoutMs: string;
  setRuntimeMemberTimeoutMs: (value: string) => void;
  handleAddRuntimeMember: () => void;
  runApprovalMemberId: string;
  setRunApprovalMemberId: (value: string) => void;
  runApprovalDecision: "approve" | "deny";
  setRunApprovalDecision: (value: "approve" | "deny") => void;
  runApprovalRuleId: string;
  setRunApprovalRuleId: (value: string) => void;
  runApprovalReason: string;
  setRunApprovalReason: (value: string) => void;
  runApprovals: InlineApproval[];
  setRunApprovals: React.Dispatch<React.SetStateAction<InlineApproval[]>>;
  handleAddRunApproval: () => void;
  runError: string | null;
  runQuorum: { rules?: Array<{ rule_id?: string; min_approvals?: number; approved?: number; missing?: number }> } | null;
  runResult: any | null;
};

export default function TeamRunCreatePanel(props: TeamRunCreatePanelProps) {
  const canCreate = props.canQuery && !!props.teamId;
  return (
    <>
      <div className="text-xs font-semibold text-white/80">Team run</div>
      <div className="text-[11px] text-white/50">Creates a run across team members; prompt is required.</div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Prompt</FieldLabel>
        <input
          className="min-w-[240px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runPrompt}
          onChange={(e) => props.setRunPrompt(e.target.value)}
          placeholder="Summarize today’s alerts"
        />
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Model</FieldLabel>
        <input
          className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runModel}
          onChange={(e) => props.setRunModel(e.target.value)}
          placeholder="optional"
        />
        <FieldLabel>Tools</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runTools}
          onChange={(e) => props.setRunTools(e.target.value)}
        >
          <option value="none">none</option>
          <option value="basic">basic</option>
          <option value="host">host</option>
        </select>
        <FieldLabel>Role</FieldLabel>
        <input
          className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runRole}
          onChange={(e) => props.setRunRole(e.target.value)}
          placeholder="planner"
        />
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Roles</FieldLabel>
        <input
          className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runRoles}
          onChange={(e) => props.setRunRoles(e.target.value)}
          placeholder="planner,executor"
        />
        <FieldLabel>Max concurrency</FieldLabel>
        <input
          className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runConcurrency}
          onChange={(e) => props.setRunConcurrency(e.target.value)}
        />
        <FieldLabel>Timeout ms</FieldLabel>
        <input
          className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runTimeoutMs}
          onChange={(e) => props.setRunTimeoutMs(e.target.value)}
        />
        <FieldLabel>Mode</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runMode}
          onChange={(e) => props.setRunMode(e.target.value)}
        >
          <option value="async">async</option>
          <option value="sync">sync</option>
        </select>
        <FieldLabel>Quorum</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runQuorumMode}
          onChange={(e) => props.setRunQuorumMode(e.target.value)}
        >
          <option value="auto">auto</option>
          <option value="off">off</option>
        </select>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canCreate || props.runBusy}
          onClick={() => props.onCreateRun()}
        >
          {props.runBusy ? "Submitting…" : "Create run"}
        </button>
      </div>
      <div className="text-[11px] text-white/50">
        Async mode dispatches `run_async` per member so the run continues if the UI disconnects.
      </div>
      <TeamRunCreateAdvancedSection {...props} />
      <TeamRunCreateRuntimeSection {...props} />
      <TeamRunCreateApprovalsSection {...props} />
      {props.runError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {props.runError}
        </div>
      ) : null}
      {props.runQuorum?.rules && props.runQuorum.rules.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          {props.runQuorum.rules.map((r, idx) => (
            <div key={`quorum-eval-${r.rule_id || idx}`}>
              {r.rule_id ? `rule ${r.rule_id}` : "rule"} · min {r.min_approvals ?? "?"} · approved {r.approved ?? 0} · missing {r.missing ?? 0}
            </div>
          ))}
        </div>
      ) : null}
      {props.runResult ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          run id {String(props.runResult?.team_run_id || "unknown")} · status {String(props.runResult?.status || "")}
          {props.runResult?.mode ? ` · mode ${String(props.runResult.mode)}` : ""}
        </div>
      ) : null}
    </>
  );
}
