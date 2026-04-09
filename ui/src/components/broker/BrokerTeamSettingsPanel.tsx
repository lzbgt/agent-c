import React from "react";
import FieldLabel from "../FieldLabel";
import type { TeamRow } from "./teamConsoleTypes";
import TeamRolePlanEditor from "./TeamRolePlanEditor";
import type { RoleGraphEdge } from "./teamRunUtils";

type BrokerTeamSettingsPanelProps = {
  canQuery: boolean;
  teamId: string;
  teamDetails: TeamRow | null;
  teamEditName: string;
  teamEditTags: string;
  teamEditPolicyRef: string;
  teamEditSharedScope: string;
  teamEditSharedMode: string;
  teamEditMetaJson: string;
  teamEditRoleOverridesJson: string;
  teamRoleInstructions: Record<string, string>;
  teamRolePromptMode: string;
  teamRoleGraphEdges: RoleGraphEdge[];
  rolePlanOptions: string[];
  teamEditBusy: boolean;
  teamEditError: string | null;
  onLoadTeamEdits: () => void;
  onTeamEditNameChange: (next: string) => void;
  onTeamEditTagsChange: (next: string) => void;
  onTeamEditPolicyRefChange: (next: string) => void;
  onTeamEditSharedScopeChange: (next: string) => void;
  onTeamEditSharedModeChange: (next: string) => void;
  onTeamEditMetaJsonChange: (next: string) => void;
  onTeamEditRoleOverridesJsonChange: (next: string) => void;
  onRoleInstructionsChange: (next: Record<string, string>) => void;
  onRolePromptModeChange: (next: string) => void;
  onRoleGraphEdgesChange: (next: RoleGraphEdge[]) => void;
  onUpdateTeam: () => void;
};

export default function BrokerTeamSettingsPanel(props: BrokerTeamSettingsPanelProps) {
  const {
    canQuery,
    teamId,
    teamDetails,
    teamEditName,
    teamEditTags,
    teamEditPolicyRef,
    teamEditSharedScope,
    teamEditSharedMode,
    teamEditMetaJson,
    teamEditRoleOverridesJson,
    teamRoleInstructions,
    teamRolePromptMode,
    teamRoleGraphEdges,
    rolePlanOptions,
    teamEditBusy,
    teamEditError,
    onLoadTeamEdits,
    onTeamEditNameChange,
    onTeamEditTagsChange,
    onTeamEditPolicyRefChange,
    onTeamEditSharedScopeChange,
    onTeamEditSharedModeChange,
    onTeamEditMetaJsonChange,
    onTeamEditRoleOverridesJsonChange,
    onRoleInstructionsChange,
    onRolePromptModeChange,
    onRoleGraphEdgesChange,
    onUpdateTeam,
  } = props;

  return (
    <div data-testid="team-settings" className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Team settings</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!teamDetails}
          onClick={onLoadTeamEdits}
        >
          Load
        </button>
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Name</FieldLabel>
        <input
          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={teamEditName}
          onChange={(e) => onTeamEditNameChange(e.target.value)}
          placeholder="Team display name"
        />
        <FieldLabel>Tags</FieldLabel>
        <input
          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={teamEditTags}
          onChange={(e) => onTeamEditTagsChange(e.target.value)}
          placeholder="ops,security"
        />
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Policy ref</FieldLabel>
        <input
          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={teamEditPolicyRef}
          onChange={(e) => onTeamEditPolicyRefChange(e.target.value)}
          placeholder="policy:high-risk"
        />
        <FieldLabel>Shared memory scope</FieldLabel>
        <input
          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={teamEditSharedScope}
          onChange={(e) => onTeamEditSharedScopeChange(e.target.value)}
          placeholder="scope-id"
        />
        <FieldLabel>Shared memory mode</FieldLabel>
        <select
          className="min-w-[160px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={teamEditSharedMode}
          onChange={(e) => onTeamEditSharedModeChange(e.target.value)}
        >
          <option value="read_write">read_write</option>
          <option value="read_only">read_only</option>
        </select>
      </div>
      <div className="grid gap-1">
        <FieldLabel>Meta JSON</FieldLabel>
        <textarea
          className="min-h-[72px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          value={teamEditMetaJson}
          onChange={(e) => onTeamEditMetaJsonChange(e.target.value)}
          placeholder='{"owner_notes":"tier-1","priority":"high"}'
        />
      </div>
      <div className="grid gap-1">
        <FieldLabel>Role overrides JSON</FieldLabel>
        <textarea
          className="min-h-[72px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          value={teamEditRoleOverridesJson}
          onChange={(e) => onTeamEditRoleOverridesJsonChange(e.target.value)}
          placeholder='{"planner":{"model":"gpt-4.1-mini","tools":"basic"},"executor":{"base_url":"https://api.openai.com/v1"}}'
        />
        <div className="text-[11px] text-white/50">
          Stored under meta.role_overrides; applied to team runs unless a run overrides it.
        </div>
      </div>
      <div className="grid gap-1">
        <FieldLabel>Role plan</FieldLabel>
        <div className="rounded-md border border-white/10 bg-black/20 p-2">
          <TeamRolePlanEditor
            disabled={!canQuery || !teamId}
            roleInstructions={teamRoleInstructions}
            onRoleInstructionsChange={onRoleInstructionsChange}
            rolePromptMode={teamRolePromptMode}
            onRolePromptModeChange={onRolePromptModeChange}
            edges={teamRoleGraphEdges}
            onEdgesChange={onRoleGraphEdgesChange}
            roleOptions={rolePlanOptions}
          />
        </div>
        <div className="text-[11px] text-white/50">
          Stored under meta.role_instructions / meta.role_prompt_mode / meta.role_graph; applied to team runs by default.
        </div>
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !teamId || teamEditBusy}
          onClick={onUpdateTeam}
        >
          {teamEditBusy ? "Saving…" : "Update team"}
        </button>
        {teamEditError ? <span className="text-[11px] text-rose-200">{teamEditError}</span> : null}
      </div>
    </div>
  );
}
