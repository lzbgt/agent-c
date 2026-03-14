import React from "react";

import FieldLabel from "../FieldLabel";
import type { BrokerTeamMembersPanelProps } from "./brokerTeamMembersTypes";

type BrokerTeamMembersFormSectionProps = Pick<
  BrokerTeamMembersPanelProps,
  | "canQuery"
  | "teamIdTrimmed"
  | "membersBusy"
  | "membersError"
  | "members"
  | "memberAgentsBusy"
  | "memberAgentsError"
  | "memberAgentOptions"
  | "memberAgentDeployments"
  | "memberId"
  | "memberRole"
  | "memberStatus"
  | "memberWeight"
  | "memberCapabilities"
  | "memberAgentId"
  | "memberDeploymentId"
  | "memberBackendLabel"
  | "memberModel"
  | "memberBaseUrl"
  | "memberSummaryModel"
  | "memberTools"
  | "memberTimeoutMs"
  | "onMemberIdChange"
  | "onMemberRoleChange"
  | "onMemberStatusChange"
  | "onMemberWeightChange"
  | "onMemberCapabilitiesChange"
  | "onMemberAgentIdChange"
  | "onMemberDeploymentIdChange"
  | "onMemberBackendLabelChange"
  | "onMemberModelChange"
  | "onMemberBaseUrlChange"
  | "onMemberSummaryModelChange"
  | "onMemberToolsChange"
  | "onMemberTimeoutMsChange"
  | "onRefreshMembers"
  | "onAddMember"
  | "onRefreshMemberAgents"
  | "onAddConnectedAgents"
  | "onSetAllMemberStatus"
  | "onRemovePausedMembers"
>;

export default function BrokerTeamMembersFormSection({
  canQuery,
  teamIdTrimmed,
  membersBusy,
  membersError,
  members,
  memberAgentsBusy,
  memberAgentsError,
  memberAgentOptions,
  memberAgentDeployments,
  memberId,
  memberRole,
  memberStatus,
  memberWeight,
  memberCapabilities,
  memberAgentId,
  memberDeploymentId,
  memberBackendLabel,
  memberModel,
  memberBaseUrl,
  memberSummaryModel,
  memberTools,
  memberTimeoutMs,
  onMemberIdChange,
  onMemberRoleChange,
  onMemberStatusChange,
  onMemberWeightChange,
  onMemberCapabilitiesChange,
  onMemberAgentIdChange,
  onMemberDeploymentIdChange,
  onMemberBackendLabelChange,
  onMemberModelChange,
  onMemberBaseUrlChange,
  onMemberSummaryModelChange,
  onMemberToolsChange,
  onMemberTimeoutMsChange,
  onRefreshMembers,
  onAddMember,
  onRefreshMemberAgents,
  onAddConnectedAgents,
  onSetAllMemberStatus,
  onRemovePausedMembers,
}: BrokerTeamMembersFormSectionProps) {
  return (
    <>
      <div className="flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Team members</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !teamIdTrimmed || membersBusy}
          onClick={onRefreshMembers}
        >
          {membersBusy ? "Loading…" : "Refresh"}
        </button>
      </div>
      <div className="grid gap-2 md:grid-cols-2 xl:grid-cols-4">
        <div className="flex items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberId}
            onChange={(e) => onMemberIdChange(e.target.value)}
            placeholder="optional"
          />
        </div>
        <div className="flex items-center gap-2">
          <FieldLabel>Role</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberRole}
            onChange={(e) => onMemberRoleChange(e.target.value)}
          />
        </div>
        <div className="flex items-center gap-2">
          <FieldLabel>Status</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberStatus}
            onChange={(e) => onMemberStatusChange(e.target.value)}
          />
        </div>
        <div className="flex items-center gap-2">
          <FieldLabel>Weight</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberWeight}
            onChange={(e) => onMemberWeightChange(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy}
            onClick={onAddMember}
          >
            Add member
          </button>
        </div>
      </div>
      <div className="grid gap-2 md:grid-cols-2 xl:grid-cols-[minmax(200px,1fr)_minmax(200px,1fr)_minmax(160px,1fr)_auto_auto]">
        <div className="flex items-center gap-2">
          <FieldLabel>Agent ID</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberAgentId}
            onChange={(e) => onMemberAgentIdChange(e.target.value)}
            placeholder="agent1"
          />
        </div>
        <div className="flex items-center gap-2">
          <FieldLabel>Agent pick</FieldLabel>
          <select
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberAgentId}
            onChange={(e) => onMemberAgentIdChange(e.target.value)}
          >
            <option value="">(select agent)</option>
            {memberAgentOptions.map((agent) => {
              const aid = String(agent?.agent_id || "");
              if (!aid) return null;
              const suffix = agent?.connected ? " · connected" : "";
              return (
                <option key={`member-agent-${aid}`} value={aid}>
                  {aid}
                  {suffix}
                </option>
              );
            })}
          </select>
        </div>
        <div className="flex items-center gap-2">
          <FieldLabel>Deployment</FieldLabel>
          <select
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberDeploymentId}
            onChange={(e) => onMemberDeploymentIdChange(e.target.value)}
            disabled={memberAgentDeployments.length === 0}
          >
            <option value="">(default)</option>
            {memberAgentDeployments.map((dep, idx) => {
              const depId = dep?.deployment_id ? String(dep.deployment_id) : "";
              return (
                <option key={`member-dep-${depId || idx}`} value={depId}>
                  {depId || `deployment-${idx + 1}`}
                </option>
              );
            })}
          </select>
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || memberAgentsBusy}
          onClick={onRefreshMemberAgents}
        >
          {memberAgentsBusy ? "Loading…" : "Refresh agents"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !teamIdTrimmed || membersBusy || memberAgentsBusy}
          onClick={onAddConnectedAgents}
        >
          Add connected agents
        </button>
        {memberAgentsError ? <span className="text-[11px] text-rose-200">{memberAgentsError}</span> : null}
      </div>
      <details className="rounded-md border border-white/10 bg-black/30 px-2 py-2">
        <summary className="cursor-pointer text-[11px] text-white/60">Advanced overrides</summary>
        <div className="mt-2 grid gap-2 md:grid-cols-2 xl:grid-cols-3">
          <div className="flex items-center gap-2">
            <FieldLabel>Capabilities</FieldLabel>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberCapabilities}
              onChange={(e) => onMemberCapabilitiesChange(e.target.value)}
              placeholder="vision,audio"
            />
          </div>
          <div className="flex items-center gap-2">
            <FieldLabel>Backend label</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberBackendLabel}
              onChange={(e) => onMemberBackendLabelChange(e.target.value)}
              placeholder="openrouter-main"
            />
          </div>
          <div className="flex items-center gap-2">
            <FieldLabel>Model</FieldLabel>
            <input
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberModel}
              onChange={(e) => onMemberModelChange(e.target.value)}
              placeholder="optional"
            />
          </div>
          <div className="flex items-center gap-2 md:col-span-2 xl:col-span-3">
            <FieldLabel>Base URL</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberBaseUrl}
              onChange={(e) => onMemberBaseUrlChange(e.target.value)}
              placeholder="https://api.openai.com/v1"
            />
          </div>
          <div className="flex items-center gap-2">
            <FieldLabel>Summary model</FieldLabel>
            <input
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberSummaryModel}
              onChange={(e) => onMemberSummaryModelChange(e.target.value)}
              placeholder="optional"
            />
          </div>
          <div className="flex items-center gap-2">
            <FieldLabel>Tools</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberTools}
              onChange={(e) => onMemberToolsChange(e.target.value)}
            >
              <option value="">inherit</option>
              <option value="none">none</option>
              <option value="basic">basic</option>
              <option value="host">host</option>
            </select>
          </div>
          <div className="flex items-center gap-2">
            <FieldLabel>Timeout ms</FieldLabel>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={memberTimeoutMs}
              onChange={(e) => onMemberTimeoutMsChange(e.target.value)}
              placeholder="60000"
            />
          </div>
        </div>
      </details>
      {membersError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {membersError}
        </div>
      ) : null}
      {members && members.length > 0 ? (
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <span>Bulk status:</span>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            disabled={!canQuery || membersBusy}
            onClick={() => onSetAllMemberStatus("paused")}
          >
            Pause all
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            disabled={!canQuery || membersBusy}
            onClick={() => onSetAllMemberStatus("active")}
          >
            Resume all
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            disabled={!canQuery || membersBusy}
            onClick={onRemovePausedMembers}
          >
            Remove paused
          </button>
        </div>
      ) : null}
    </>
  );
}
