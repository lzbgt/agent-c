import React from "react";
import FieldLabel from "../FieldLabel";
import type { TeamRunCreatePanelProps } from "./TeamRunCreatePanel";

export default function TeamRunCreateRuntimeSection(props: TeamRunCreatePanelProps) {
  return (
    <details className="rounded-md border border-white/10 bg-black/20 p-2">
      <summary className="cursor-pointer text-[11px] text-white/70">Runtime members (optional)</summary>
      <div className="mt-2 grid gap-2">
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
                      {props.runtimeTeamDiff.mismatched.map((row, idx) => {
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
                disabled={!props.canQuery || props.runtimeAgentsBusy || props.runRolePlanOptions.length === 0}
                onClick={() => props.handleAllocateRoleRuntimeMembers()}
              >
                Allocate by roles
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!props.canQuery || props.runtimeSaveBusy || props.runtimeMembersPreview.items.length === 0}
                onClick={() => props.handleSaveRuntimeMembers()}
              >
                {props.runtimeSaveBusy ? "Saving…" : "Save to team"}
              </button>
              {props.runtimeAgentsError ? <span className="text-[11px] text-rose-200">{props.runtimeAgentsError}</span> : null}
            </div>
            {props.runtimeSaveError ? <div className="text-[11px] text-rose-200">{props.runtimeSaveError}</div> : null}
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
      </div>
    </details>
  );
}
