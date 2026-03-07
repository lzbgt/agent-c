import React from "react";
import FieldLabel from "../FieldLabel";
import SectionCard from "./BrokerTeamSectionCard";
import { fmtTs } from "./teamRunUtils";

export type BrokerTeamMembersPanelProps = {
  canQuery: boolean;
  teamIdTrimmed: string;
  membersBusy: boolean;
  membersError: string | null;
  members: Array<Record<string, any>> | null;
  memberAgentsBusy: boolean;
  memberAgentsError: string | null;
  memberAgentOptions: Array<Record<string, any>>;
  memberAgentDeployments: Array<Record<string, any>>;
  memberEditAgentDeployments: Array<Record<string, any>>;
  memberId: string;
  memberRole: string;
  memberStatus: string;
  memberWeight: string;
  memberCapabilities: string;
  memberAgentId: string;
  memberDeploymentId: string;
  memberBackendLabel: string;
  memberModel: string;
  memberBaseUrl: string;
  memberSummaryModel: string;
  memberTools: string;
  memberTimeoutMs: string;
  memberEditId: string;
  memberEditRole: string;
  memberEditStatus: string;
  memberEditWeight: string;
  memberEditCapabilities: string;
  memberEditMetaJson: string;
  memberEditBackendLabel: string;
  memberEditModel: string;
  memberEditBaseUrl: string;
  memberEditSummaryModel: string;
  memberEditTools: string;
  memberEditTimeoutMs: string;
  memberEditAgentId: string;
  memberEditDeploymentId: string;
  memberEditBusy: boolean;
  memberEditError: string | null;
  onMemberIdChange: (next: string) => void;
  onMemberRoleChange: (next: string) => void;
  onMemberStatusChange: (next: string) => void;
  onMemberWeightChange: (next: string) => void;
  onMemberCapabilitiesChange: (next: string) => void;
  onMemberAgentIdChange: (next: string) => void;
  onMemberDeploymentIdChange: (next: string) => void;
  onMemberBackendLabelChange: (next: string) => void;
  onMemberModelChange: (next: string) => void;
  onMemberBaseUrlChange: (next: string) => void;
  onMemberSummaryModelChange: (next: string) => void;
  onMemberToolsChange: (next: string) => void;
  onMemberTimeoutMsChange: (next: string) => void;
  onMemberEditRoleChange: (next: string) => void;
  onMemberEditStatusChange: (next: string) => void;
  onMemberEditWeightChange: (next: string) => void;
  onMemberEditCapabilitiesChange: (next: string) => void;
  onMemberEditBackendLabelChange: (next: string) => void;
  onMemberEditModelChange: (next: string) => void;
  onMemberEditBaseUrlChange: (next: string) => void;
  onMemberEditSummaryModelChange: (next: string) => void;
  onMemberEditToolsChange: (next: string) => void;
  onMemberEditTimeoutMsChange: (next: string) => void;
  onMemberEditMetaJsonChange: (next: string) => void;
  onMemberEditAgentIdChange: (next: string) => void;
  onMemberEditDeploymentIdChange: (next: string) => void;
  onRefreshMembers: () => void;
  onAddMember: () => void;
  onRefreshMemberAgents: () => void;
  onAddConnectedAgents: () => void;
  onToggleMemberStatus: (member: Record<string, any>) => void;
  onEditMember: (member: Record<string, any>) => void;
  onDeleteMember: (memberId: string) => void;
  onSaveMemberEdit: () => void;
  onCancelMemberEdit: () => void;
  onSetAllMemberStatus: (status: "active" | "paused") => void;
  onRemovePausedMembers: () => void;
};

export default function BrokerTeamMembersPanel(props: BrokerTeamMembersPanelProps) {
  const {
    canQuery,
    teamIdTrimmed,
    membersBusy,
    membersError,
    members,
    memberAgentsBusy,
    memberAgentsError,
    memberAgentOptions,
    memberAgentDeployments,
    memberEditAgentDeployments,
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
    memberEditId,
    memberEditRole,
    memberEditStatus,
    memberEditWeight,
    memberEditCapabilities,
    memberEditMetaJson,
    memberEditBackendLabel,
    memberEditModel,
    memberEditBaseUrl,
    memberEditSummaryModel,
    memberEditTools,
    memberEditTimeoutMs,
    memberEditAgentId,
    memberEditDeploymentId,
    memberEditBusy,
    memberEditError,
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
    onMemberEditRoleChange,
    onMemberEditStatusChange,
    onMemberEditWeightChange,
    onMemberEditCapabilitiesChange,
    onMemberEditBackendLabelChange,
    onMemberEditModelChange,
    onMemberEditBaseUrlChange,
    onMemberEditSummaryModelChange,
    onMemberEditToolsChange,
    onMemberEditTimeoutMsChange,
    onMemberEditMetaJsonChange,
    onMemberEditAgentIdChange,
    onMemberEditDeploymentIdChange,
    onRefreshMembers,
    onAddMember,
    onRefreshMemberAgents,
    onAddConnectedAgents,
    onToggleMemberStatus,
    onEditMember,
    onDeleteMember,
    onSaveMemberEdit,
    onCancelMemberEdit,
    onSetAllMemberStatus,
    onRemovePausedMembers,
  } = props;

  return (
    <SectionCard title="Team members" description="Add members, update status, or edit overrides.">
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
      {members && members.length > 0 ? (
        <div className="grid max-h-[55vh] gap-2 overflow-y-auto pr-1">
          {members.map((m, idx) => {
            const mid = String(m?.member_id || "");
            const meta = m?.meta && typeof m.meta === "object" ? (m.meta as Record<string, any>) : null;
            const backendLabel = meta?.backend_label ? String(meta.backend_label) : "";
            const overridesRaw = meta?.run_overrides && typeof meta.run_overrides === "object" ? meta.run_overrides : null;
            const overrideBits: string[] = [];
            if (overridesRaw && typeof overridesRaw === "object") {
              const model = (overridesRaw as any).model ? String((overridesRaw as any).model) : "";
              const baseUrl = (overridesRaw as any).base_url ? String((overridesRaw as any).base_url) : "";
              const summaryModel = (overridesRaw as any).summary_model ? String((overridesRaw as any).summary_model) : "";
              const tools = (overridesRaw as any).tools ? String((overridesRaw as any).tools) : "";
              const timeoutMs = (overridesRaw as any).timeout_ms;
              const maxSteps = (overridesRaw as any).max_steps;
              const streamAssistant = (overridesRaw as any).stream_assistant;
              if (model) overrideBits.push(`model ${model}`);
              if (summaryModel) overrideBits.push(`summary ${summaryModel}`);
              if (baseUrl) overrideBits.push(`base ${baseUrl}`);
              if (tools) overrideBits.push(`tools ${tools}`);
              if (Number.isFinite(timeoutMs)) overrideBits.push(`timeout ${timeoutMs}ms`);
              if (Number.isFinite(maxSteps)) overrideBits.push(`max_steps ${maxSteps}`);
              if (typeof streamAssistant === "boolean") {
                overrideBits.push(`stream ${streamAssistant ? "on" : "off"}`);
              }
            }
            const infoBits: string[] = [];
            if (typeof m?.weight === "number") infoBits.push(`weight ${m.weight}`);
            const caps = Array.isArray(m?.capabilities) ? m.capabilities.map((c) => String(c).trim()).filter(Boolean) : [];
            if (caps.length > 0) infoBits.push(`caps ${caps.join(",")}`);
            if (backendLabel) infoBits.push(`backend ${backendLabel}`);
            if (overrideBits.length > 0) infoBits.push(`overrides ${overrideBits.join(", ")}`);
            return (
              <div key={`member-${mid}-${idx}`} className="grid gap-2">
                <div className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                  <div className="text-[11px] text-white/70">
                    <div>
                      <span className="text-white/90">{mid || "member"}</span>
                      {m?.role ? ` · role ${m.role}` : ""}
                      {m?.status ? ` · ${m.status}` : ""}
                      {m?.agent_id ? ` · agent ${m.agent_id}` : ""}
                      {m?.deployment_id ? ` · dep ${m.deployment_id}` : ""}
                      {m?.created_unix_ms ? ` · ${fmtTs(m.created_unix_ms)}` : ""}
                    </div>
                    {infoBits.length > 0 ? <div className="text-[10px] text-white/50">{infoBits.join(" · ")}</div> : null}
                  </div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => onToggleMemberStatus(m)}
                    >
                      {String(m?.status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => onEditMember(m)}
                    >
                      Edit
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => onDeleteMember(mid)}
                    >
                      Remove
                    </button>
                  </div>
                </div>
                {memberEditId === mid ? (
                  <div data-testid="team-member-edit" className="rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
                    <div className="flex flex-wrap items-center gap-2">
                      <FieldLabel>Role</FieldLabel>
                      <input
                        className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-role"
                        value={memberEditRole}
                        onChange={(e) => onMemberEditRoleChange(e.target.value)}
                      />
                      <FieldLabel>Status</FieldLabel>
                      <select
                        className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-status"
                        value={memberEditStatus}
                        onChange={(e) => onMemberEditStatusChange(e.target.value)}
                      >
                        <option value="active">active</option>
                        <option value="paused">paused</option>
                        <option value="disabled">disabled</option>
                      </select>
                      <FieldLabel>Weight</FieldLabel>
                      <input
                        className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-weight"
                        value={memberEditWeight}
                        onChange={(e) => onMemberEditWeightChange(e.target.value)}
                        placeholder="1"
                      />
                    </div>
                    <div className="mt-2 flex flex-wrap items-center gap-2">
                      <FieldLabel>Agent ID</FieldLabel>
                      <input
                        className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-agent-id"
                        value={memberEditAgentId}
                        onChange={(e) => onMemberEditAgentIdChange(e.target.value)}
                        placeholder="agent1"
                      />
                      <FieldLabel>Agent pick</FieldLabel>
                      <select
                        className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-agent-pick"
                        value={memberEditAgentId}
                        onChange={(e) => onMemberEditAgentIdChange(e.target.value)}
                      >
                        <option value="">(select agent)</option>
                        {memberAgentOptions.map((agent) => {
                          const aid = String(agent?.agent_id || "");
                          if (!aid) return null;
                          const suffix = agent?.connected ? " · connected" : "";
                          return (
                            <option key={`member-edit-agent-${aid}`} value={aid}>
                              {aid}
                              {suffix}
                            </option>
                          );
                        })}
                      </select>
                      <FieldLabel>Deployment</FieldLabel>
                      <select
                        className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                        data-testid="team-member-edit-deployment"
                        value={memberEditDeploymentId}
                        onChange={(e) => onMemberEditDeploymentIdChange(e.target.value)}
                        disabled={memberEditAgentDeployments.length === 0}
                      >
                        <option value="">(default)</option>
                        {memberEditAgentDeployments.map((dep, idx) => {
                          const depId = dep?.deployment_id ? String(dep.deployment_id) : "";
                          return (
                            <option key={`member-edit-dep-${depId || idx}`} value={depId}>
                              {depId || `deployment-${idx + 1}`}
                            </option>
                          );
                        })}
                      </select>
                    </div>
                    <div className="mt-2 flex flex-wrap items-center gap-2">
                      <details className="w-full rounded-md border border-white/10 bg-black/20 px-2 py-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">Advanced overrides</summary>
                        <div className="mt-2 flex flex-wrap items-center gap-2">
                          <FieldLabel>Capabilities</FieldLabel>
                          <input
                            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-caps"
                            value={memberEditCapabilities}
                            onChange={(e) => onMemberEditCapabilitiesChange(e.target.value)}
                            placeholder="vision,audio"
                          />
                          <FieldLabel>Backend label</FieldLabel>
                          <input
                            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-backend"
                            value={memberEditBackendLabel}
                            onChange={(e) => onMemberEditBackendLabelChange(e.target.value)}
                            placeholder="openrouter-main"
                          />
                          <FieldLabel>Model</FieldLabel>
                          <input
                            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-model"
                            value={memberEditModel}
                            onChange={(e) => onMemberEditModelChange(e.target.value)}
                            placeholder="gpt-4.1-mini"
                          />
                          <FieldLabel>Tools</FieldLabel>
                          <input
                            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-tools"
                            value={memberEditTools}
                            onChange={(e) => onMemberEditToolsChange(e.target.value)}
                            placeholder="basic"
                          />
                        </div>
                        <div className="mt-2 flex flex-wrap items-center gap-2">
                          <FieldLabel>Base URL</FieldLabel>
                          <input
                            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-base-url"
                            value={memberEditBaseUrl}
                            onChange={(e) => onMemberEditBaseUrlChange(e.target.value)}
                            placeholder="https://api.openai.com/v1"
                          />
                          <FieldLabel>Summary model</FieldLabel>
                          <input
                            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-summary-model"
                            value={memberEditSummaryModel}
                            onChange={(e) => onMemberEditSummaryModelChange(e.target.value)}
                            placeholder="gpt-4.1-mini"
                          />
                          <FieldLabel>Timeout ms</FieldLabel>
                          <input
                            className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                            data-testid="team-member-edit-timeout"
                            value={memberEditTimeoutMs}
                            onChange={(e) => onMemberEditTimeoutMsChange(e.target.value)}
                            placeholder="60000"
                          />
                        </div>
                        <div className="mt-2 grid gap-1">
                          <FieldLabel>Meta JSON</FieldLabel>
                          <textarea
                            className="min-h-[72px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                            data-testid="team-member-edit-meta"
                            value={memberEditMetaJson}
                            onChange={(e) => onMemberEditMetaJsonChange(e.target.value)}
                            placeholder='{"backend_label":"openrouter-main"}'
                          />
                        </div>
                      </details>
                    </div>
                    <div className="mt-2 flex flex-wrap items-center gap-2">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        disabled={!canQuery || memberEditBusy}
                        onClick={onSaveMemberEdit}
                      >
                        {memberEditBusy ? "Saving…" : "Save"}
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                        type="button"
                        onClick={onCancelMemberEdit}
                      >
                        Cancel
                      </button>
                      {memberEditError ? <span className="text-[11px] text-rose-200">{memberEditError}</span> : null}
                    </div>
                  </div>
                ) : null}
              </div>
            );
          })}
        </div>
      ) : null}
    </SectionCard>
  );
}
