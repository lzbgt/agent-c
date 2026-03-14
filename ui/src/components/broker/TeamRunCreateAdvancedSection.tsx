import React from "react";
import FieldLabel from "../FieldLabel";
import TeamRolePlanEditor from "./TeamRolePlanEditor";
import type { TeamRunCreatePanelProps } from "./TeamRunCreatePanel";

export default function TeamRunCreateAdvancedSection(props: TeamRunCreatePanelProps) {
  const canCreate = props.canQuery && !!props.teamId;
  return (
    <details className="rounded-md border border-white/10 bg-black/20 p-2">
      <summary className="cursor-pointer text-[11px] text-white/70">Advanced run options</summary>
      <div className="mt-2 grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Shared memory scope</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runSharedMemoryScope}
            onChange={(e) => props.setRunSharedMemoryScope(e.target.value)}
            placeholder="override scope-id (optional)"
          />
          <FieldLabel>Shared memory mode</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runSharedMemoryMode}
            onChange={(e) => props.setRunSharedMemoryMode(e.target.value)}
          >
            <option value="read_write">read_write</option>
            <option value="read_only">read_only</option>
          </select>
          <span className="text-[11px] text-white/50">Overrides team shared memory for this run.</span>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <label className="flex items-center gap-2 text-[11px] text-white/70">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={props.runAutoAllocateRoles}
              onChange={(e) => props.setRunAutoAllocateRoles(e.target.checked)}
            />
            Auto-allocate runtime members by role
          </label>
          <FieldLabel>Max members</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runAutoAllocateMaxMembers}
            onChange={(e) => props.setRunAutoAllocateMaxMembers(e.target.value)}
            placeholder="optional"
          />
          <span className="text-[11px] text-white/50">Fills missing roles from connected agents when enabled.</span>
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
                : "No role instruction defaults saved on this team."}{" "}
              Enable override to send per-run role instructions.
            </div>
          )}
        </div>
      </div>
    </details>
  );
}
