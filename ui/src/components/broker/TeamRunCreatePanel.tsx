import React from "react";
import FieldLabel from "../FieldLabel";
import TeamRolePlanEditor from "./TeamRolePlanEditor";

export type InlineApproval = {
  member_id: string;
  decision: "approve" | "deny";
  rule_id?: string;
  reason?: string;
};

type TeamRunCreatePanelProps = {
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
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Run overrides</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.runOverridesMode}
          onChange={(e) => props.setRunOverridesMode(e.target.value)}
        >
          <option value="off">off</option>
          <option value="member_meta">member meta</option>
          <option value="explicit">explicit</option>
        </select>
        <span className="text-[11px] text-white/50">Apply per-member backend overrides when enabled.</span>
      </div>
      {props.runOverridesMode === "explicit" ? (
        <div className="grid gap-1">
          <FieldLabel>Member overrides JSON</FieldLabel>
          <textarea
            className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={props.runMemberOverridesJson}
            onChange={(e) => props.setRunMemberOverridesJson(e.target.value)}
            placeholder='{"member_1":{"model":"gpt-4.1-mini","base_url":"https://api.openai.com/v1","tools":"basic"}}'
          />
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.onSeedExplicitOverrides()}
            >
              Seed from team members
            </button>
          </div>
          <div className="text-[11px] text-white/50">
            Allowed fields: model, base_url, summary_model, tools, timeout_ms, max_steps, stream_assistant.
          </div>
        </div>
      ) : null}
      <div className="grid gap-1">
        <FieldLabel>Role overrides JSON (optional)</FieldLabel>
        <textarea
          className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          value={props.runRoleOverridesJson}
          onChange={(e) => props.setRunRoleOverridesJson(e.target.value)}
          placeholder='{"planner":{"model":"gpt-4.1-mini","tools":"basic"},"executor":{"base_url":"https://api.openai.com/v1"}}'
        />
        <div className="text-[11px] text-white/50">
          Role overrides apply before member overrides and use the same allowlist. If empty, broker uses team defaults.
        </div>
        {props.teamRoleOverrideKeys.length > 0 ? (
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.onSeedRoleOverrides()}
            >
              Seed from team defaults
            </button>
            <span className="text-[11px] text-white/50">Defaults: {props.teamRoleOverrideKeys.join(", ")}</span>
          </div>
        ) : (
          <div className="text-[11px] text-white/40">No role override defaults saved on this team.</div>
        )}
      </div>
      <div className="grid gap-1">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Role prompts (optional)</FieldLabel>
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input
              type="checkbox"
              checked={props.runRoleInstructionsOverride}
              onChange={(e) => props.setRunRoleInstructionsOverride(e.target.checked)}
            />
            override defaults
          </label>
          {props.teamRoleInstructionKeys.length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.onSeedRoleInstructions()}
            >
              Seed from team defaults
            </button>
          ) : null}
        </div>
        {props.runRoleInstructionsOverride ? (
          <div className="rounded-md border border-white/10 bg-black/20 p-2">
            <TeamRolePlanEditor
              disabled={!canCreate}
              roleInstructions={props.runRoleInstructions}
              onRoleInstructionsChange={props.setRunRoleInstructions}
              rolePromptMode={props.runRolePromptMode}
              onRolePromptModeChange={props.setRunRolePromptMode}
              showEdges={false}
              roleOptions={props.runRolePlanOptions}
            />
          </div>
        ) : (
          <div className="text-[11px] text-white/40">
            {props.teamRoleInstructionKeys.length > 0
              ? `Defaults: ${props.teamRoleInstructionKeys.join(", ")}`
              : "No role instruction defaults saved on this team."} {" "}
            Enable override to send per-run role instructions.
          </div>
        )}
      </div>
      <div className="grid gap-1">
        <FieldLabel>Runtime members JSON (optional)</FieldLabel>
        <textarea
          className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          data-testid="team-run-runtime-json"
          value={props.runRuntimeMembersJson}
          onChange={(e) => props.setRunRuntimeMembersJson(e.target.value)}
          placeholder='[{"member_id":"rt-1","agent_id":"agent_a","role":"executor","capabilities":["vision"],"meta":{"backend_label":"openai-mini"}}]'
        />
        <div className="text-[11px] text-white/50">
          Each entry needs agent_id + role; member_id is optional (required for explicit overrides).
        </div>
        <div className="text-[11px] text-white/50">
          Runtime members are per-run and let an orchestrator add/pause agents dynamically; use "Save to team" to persist.
        </div>
        {props.runtimeMembersPreview.error ? (
          <div className="text-[11px] text-rose-200">{props.runtimeMembersPreview.error}</div>
        ) : null}
        {props.runtimeMembersPreview.items.length > 0 ? (
          <div className="grid gap-2">
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <span>Bulk status:</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleSetAllRuntimeStatus("paused")}
              >
                Pause all
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleSetAllRuntimeStatus("active")}
              >
                Resume all
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleRemovePausedRuntimeMembers()}
              >
                Remove paused
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleCompactRuntimeMembers()}
              >
                Compact JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleCopyRuntimeMembers()}
              >
                Copy JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.runtimeImportRef.current?.click()}
              >
                Import JSON
              </button>
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                <input
                  type="checkbox"
                  checked={props.runtimeImportMerge}
                  onChange={(e) => props.setRuntimeImportMerge(e.target.checked)}
                />
                merge
              </label>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleDownloadRuntimeMembers()}
              >
                Download JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => props.handleExportTeamMembers()}
              >
                Export team
              </button>
              <input
                ref={props.runtimeImportRef}
                className="hidden"
                type="file"
                accept="application/json,.json"
                onChange={props.handleImportRuntimeMembers}
              />
            </div>
            {props.runtimeMembersPreview.items.map((item, idx) => {
              const memberId = item?.member_id ? String(item.member_id) : "";
              const agentId = item?.agent_id ? String(item.agent_id) : "";
              const role = item?.role ? String(item.role) : "";
              const label = memberId ? `${memberId}` : agentId ? `agent ${agentId}` : "runtime member";
              const status = item?.status ? String(item.status).toLowerCase() : "active";
              return (
                <div
                  key={`runtime-preview-${label}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div>
                    <span className="text-white/90">{label}</span>
                    {agentId && memberId ? ` · agent ${agentId}` : ""}
                    {role ? ` · role ${role}` : ""}
                    {status && status !== "active" ? ` · ${status}` : ""}
                  </div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => props.handleToggleRuntimeMemberStatus(idx)}
                    >
                      {status === "paused" ? "Resume" : "Pause"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => props.handleRemoveRuntimeMember(idx)}
                    >
                      Remove
                    </button>
                  </div>
                </div>
              );
            })}
            {props.runtimeMembersPreview.items.length > 0 ? (
              <div className="mt-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                <div>
                  team diff · runtime-only {props.runtimeTeamDiff.runtimeOnly.length} · team-only {props.runtimeTeamDiff.teamOnly.length} · mismatched {props.runtimeTeamDiff.mismatched.length}
                </div>
                {props.runtimeTeamDiff.runtimeOnly.length > 0 ? (
                  <div>
                    runtime-only:
                    {props.runtimeTeamDiff.runtimeOnly.map((item, idx) => {
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `runtime-${idx + 1}`;
                      return <div key={`runtime-only-${label}-${idx}`}>{label}</div>;
                    })}
                  </div>
                ) : null}
                {props.runtimeTeamDiff.teamOnly.length > 0 ? (
                  <div>
                    team-only:
                    {props.runtimeTeamDiff.teamOnly.map((item, idx) => {
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `team-${idx + 1}`;
                      return <div key={`team-only-${label}-${idx}`}>{label}</div>;
                    })}
                  </div>
                ) : null}
                {props.runtimeTeamDiff.mismatched.length > 0 ? (
                  <div>
                    mismatched:
                    {props.runtimeTeamDiff.mismatched.map((row: any, idx: number) => {
                      const item = row?.item ?? {};
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `runtime-${idx + 1}`;
                      return (
                        <div key={`runtime-mismatch-${label}-${idx}`}>
                          {label} · {Array.isArray(row?.diffs) ? row.diffs.join(", ") : "diff"}
                        </div>
                      );
                    })}
                  </div>
                ) : null}
              </div>
            ) : null}
          </div>
        ) : null}
        <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
          <div className="text-[11px] text-white/70">Quick add runtime member</div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Member ID</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberId}
              onChange={(e) => props.setRuntimeMemberId(e.target.value)}
              placeholder="optional"
            />
            <FieldLabel>Agent ID</FieldLabel>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberAgentId}
              onChange={(e) => props.setRuntimeMemberAgentId(e.target.value)}
              placeholder="agent1"
            />
            <FieldLabel>Role</FieldLabel>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberRole}
              onChange={(e) => props.setRuntimeMemberRole(e.target.value)}
              placeholder="executor"
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Agent pick</FieldLabel>
            <select
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberAgentId}
              onChange={(e) => props.setRuntimeMemberAgentId(e.target.value)}
            >
              <option value="">(select agent)</option>
              {props.runtimeAgentOptions.map((agent) => {
                const id = String(agent?.agent_id || "");
                const connected = agent?.connected === true;
                const depCount = Array.isArray(agent?.deployments) ? agent.deployments.length : 0;
                const label = id
                  ? `${id}${connected ? " · connected" : ""}${depCount ? ` · ${depCount} dep` : ""}`
                  : "agent";
                return (
                  <option key={`runtime-agent-${id}`} value={id}>
                    {label}
                  </option>
                );
              })}
            </select>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || props.runtimeAgentsBusy}
              onClick={() => props.refreshRuntimeAgents()}
            >
              {props.runtimeAgentsBusy ? "Loading…" : "Refresh agents"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || props.runtimeAgentsBusy}
              onClick={() => props.handleAddConnectedAgents()}
            >
              Add connected agents
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || props.runtimeSaveBusy || props.runtimeMembersPreview.items.length === 0}
              onClick={() => props.handleSaveRuntimeMembers()}
            >
              {props.runtimeSaveBusy ? "Saving…" : "Save to team"}
            </button>
            {props.runtimeAgentsError ? (
              <span className="text-[11px] text-rose-200">{props.runtimeAgentsError}</span>
            ) : null}
          </div>
          {props.runtimeSaveError ? (
            <div className="text-[11px] text-rose-200">{props.runtimeSaveError}</div>
          ) : null}
          {props.runtimeMembersPreview.items.length > 0 ? (
            <div className="grid gap-1 text-[11px] text-white/50">
              <div>
                save preview: {props.runtimeSavePreview.newMembers.length} new · {props.runtimeSavePreview.skipped.length} skipped · {props.runtimeSavePreview.invalid.length} invalid
              </div>
              {props.runtimeSavePreview.invalid.length > 0 ? (
                <div>
                  invalid:
                  <button
                    className="ml-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => props.handleFixInvalidRuntimeMembers()}
                  >
                    Fix invalid
                  </button>
                  {props.runtimeSavePreview.invalid.map((row, idx) => {
                    const item = row?.item ?? {};
                    const label = item?.member_id
                      ? String(item.member_id)
                      : item?.agent_id
                      ? `agent ${String(item.agent_id)}`
                      : `runtime-${idx + 1}`;
                    return (
                      <div key={`runtime-invalid-${label}-${idx}`}>
                        {label} · {row?.reason || "invalid"}
                      </div>
                    );
                  })}
                </div>
              ) : null}
              {props.runtimeSavePreview.skipped.length > 0 ? (
                <div>
                  skipped:
                  {props.runtimeSavePreview.skipped.map((row, idx) => {
                    const item = row?.item ?? {};
                    const label = item?.member_id
                      ? String(item.member_id)
                      : item?.agent_id
                      ? `agent ${String(item.agent_id)}`
                      : `runtime-${idx + 1}`;
                    return (
                      <div key={`runtime-skipped-${label}-${idx}`}>
                        {label} · {row?.reason || "skipped"}
                      </div>
                    );
                  })}
                </div>
              ) : null}
            </div>
          ) : null}
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Deployment</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberDeploymentId}
              onChange={(e) => props.setRuntimeMemberDeploymentId(e.target.value)}
              placeholder="default"
            />
            <FieldLabel>Deployment pick</FieldLabel>
            <select
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberDeploymentId}
              onChange={(e) => props.setRuntimeMemberDeploymentId(e.target.value)}
              disabled={props.runtimeAgentDeployments.length === 0}
            >
              <option value="">default</option>
              {props.runtimeAgentDeployments.map((dep, idx) => {
                const depId = String(dep?.deployment_id || "");
                const connected = dep?.connected === true;
                const label = depId ? `${depId}${connected ? " · connected" : ""}` : `deployment-${idx + 1}`;
                return (
                  <option key={`runtime-dep-${depId || idx}`} value={depId}>
                    {label}
                  </option>
                );
              })}
            </select>
            <FieldLabel>Capabilities</FieldLabel>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberCapabilities}
              onChange={(e) => props.setRuntimeMemberCapabilities(e.target.value)}
              placeholder="vision,audio"
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Backend label</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberBackendLabel}
              onChange={(e) => props.setRuntimeMemberBackendLabel(e.target.value)}
              placeholder="openrouter-main"
            />
            <FieldLabel>Model</FieldLabel>
            <input
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberModel}
              onChange={(e) => props.setRuntimeMemberModel(e.target.value)}
              placeholder="optional"
            />
            <FieldLabel>Base URL</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberBaseUrl}
              onChange={(e) => props.setRuntimeMemberBaseUrl(e.target.value)}
              placeholder="https://api.openai.com/v1"
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Summary model</FieldLabel>
            <input
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberSummaryModel}
              onChange={(e) => props.setRuntimeMemberSummaryModel(e.target.value)}
              placeholder="optional"
            />
            <FieldLabel>Tools</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberTools}
              onChange={(e) => props.setRuntimeMemberTools(e.target.value)}
            >
              <option value="">inherit</option>
              <option value="none">none</option>
              <option value="basic">basic</option>
              <option value="host">host</option>
            </select>
            <FieldLabel>Timeout ms</FieldLabel>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.runtimeMemberTimeoutMs}
              onChange={(e) => props.setRuntimeMemberTimeoutMs(e.target.value)}
              placeholder="60000"
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.handleAddRuntimeMember()}
            >
              Add runtime member
            </button>
            {props.runRuntimeMembersJson.trim() ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => props.setRunRuntimeMembersJson("")}
              >
                Clear runtime members
              </button>
            ) : null}
          </div>
        </div>
      </div>
      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2" data-testid="team-inline-approvals">
        <div className="text-[11px] text-white/70">Inline approvals (optional)</div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalMemberId}
            onChange={(e) => props.setRunApprovalMemberId(e.target.value)}
            placeholder="member id"
            list="team-approvals-members"
          />
          <FieldLabel>Decision</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalDecision}
            onChange={(e) => props.setRunApprovalDecision(e.target.value as "approve" | "deny")}
          >
            <option value="approve">approve</option>
            <option value="deny">deny</option>
          </select>
          <FieldLabel>Rule ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalRuleId}
            onChange={(e) => props.setRunApprovalRuleId(e.target.value)}
            placeholder="optional"
            list="team-approvals-rules"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Reason</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalReason}
            onChange={(e) => props.setRunApprovalReason(e.target.value)}
            placeholder="optional"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canCreate || props.runBusy}
            onClick={() => props.handleAddRunApproval()}
          >
            Add approval
          </button>
          {props.runApprovals.length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => props.setRunApprovals([])}
            >
              Clear approvals
            </button>
          ) : null}
        </div>
        {props.runApprovals.length > 0 ? (
          <div className="grid gap-2">
            {props.runApprovals.map((a, idx) => (
              <div
                key={`run-approval-${a.member_id}-${a.rule_id || "any"}-${idx}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-[11px] text-white/70">
                  <span className="text-white/90">{a.member_id}</span>
                  {a.decision ? ` · ${a.decision}` : ""}
                  {a.rule_id ? ` · rule ${a.rule_id}` : ""}
                  {a.reason ? ` · ${a.reason}` : ""}
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => props.setRunApprovals((prev) => prev.filter((_, i) => i !== idx))}
                >
                  Remove
                </button>
              </div>
            ))}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No inline approvals.</div>
        )}
      </div>
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
