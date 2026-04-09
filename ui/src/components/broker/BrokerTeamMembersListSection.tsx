import React from "react";

import FieldLabel from "../FieldLabel";
import { fmtTs } from "./teamRunUtils";
import type { BrokerTeamMemberRecord, BrokerTeamMembersPanelProps } from "./brokerTeamMembersTypes";

type BrokerTeamMembersListSectionProps = Pick<
  BrokerTeamMembersPanelProps,
  | "canQuery"
  | "members"
  | "memberAgentOptions"
  | "memberEditAgentDeployments"
  | "memberEditId"
  | "memberEditRole"
  | "memberEditStatus"
  | "memberEditWeight"
  | "memberEditCapabilities"
  | "memberEditMetaJson"
  | "memberEditBackendLabel"
  | "memberEditModel"
  | "memberEditBaseUrl"
  | "memberEditSummaryModel"
  | "memberEditTools"
  | "memberEditTimeoutMs"
  | "memberEditAgentId"
  | "memberEditDeploymentId"
  | "memberEditBusy"
  | "memberEditError"
  | "onMemberEditRoleChange"
  | "onMemberEditStatusChange"
  | "onMemberEditWeightChange"
  | "onMemberEditCapabilitiesChange"
  | "onMemberEditBackendLabelChange"
  | "onMemberEditModelChange"
  | "onMemberEditBaseUrlChange"
  | "onMemberEditSummaryModelChange"
  | "onMemberEditToolsChange"
  | "onMemberEditTimeoutMsChange"
  | "onMemberEditMetaJsonChange"
  | "onMemberEditAgentIdChange"
  | "onMemberEditDeploymentIdChange"
  | "onToggleMemberStatus"
  | "onEditMember"
  | "onDeleteMember"
  | "onSaveMemberEdit"
  | "onCancelMemberEdit"
>;

function buildMemberInfoBits(member: BrokerTeamMemberRecord) {
  const meta = member?.meta && typeof member.meta === "object" ? (member.meta as Record<string, unknown>) : null;
  const backendLabel = typeof meta?.backend_label === "string" ? meta.backend_label : "";
  const overridesRaw =
    meta?.run_overrides && typeof meta.run_overrides === "object"
      ? (meta.run_overrides as Record<string, unknown>)
      : null;
  const overrideBits: string[] = [];
  if (overridesRaw) {
    const model = typeof overridesRaw.model === "string" ? overridesRaw.model : "";
    const baseUrl = typeof overridesRaw.base_url === "string" ? overridesRaw.base_url : "";
    const summaryModel = typeof overridesRaw.summary_model === "string" ? overridesRaw.summary_model : "";
    const tools = typeof overridesRaw.tools === "string" ? overridesRaw.tools : "";
    const timeoutMs = overridesRaw.timeout_ms;
    const maxSteps = overridesRaw.max_steps;
    const streamAssistant = overridesRaw.stream_assistant;
    if (model) overrideBits.push(`model ${model}`);
    if (summaryModel) overrideBits.push(`summary ${summaryModel}`);
    if (baseUrl) overrideBits.push(`base ${baseUrl}`);
    if (tools) overrideBits.push(`tools ${tools}`);
    if (Number.isFinite(timeoutMs)) overrideBits.push(`timeout ${timeoutMs}ms`);
    if (Number.isFinite(maxSteps)) overrideBits.push(`max_steps ${maxSteps}`);
    if (typeof streamAssistant === "boolean") overrideBits.push(`stream ${streamAssistant ? "on" : "off"}`);
  }
  const infoBits: string[] = [];
  if (typeof member?.weight === "number") infoBits.push(`weight ${member.weight}`);
  const caps = Array.isArray(member?.capabilities) ? member.capabilities.map((c) => String(c).trim()).filter(Boolean) : [];
  if (caps.length > 0) infoBits.push(`caps ${caps.join(",")}`);
  if (backendLabel) infoBits.push(`backend ${backendLabel}`);
  if (overrideBits.length > 0) infoBits.push(`overrides ${overrideBits.join(", ")}`);
  return infoBits;
}

function MemberEditPanel({
  canQuery,
  memberAgentOptions,
  memberEditAgentDeployments,
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
  onSaveMemberEdit,
  onCancelMemberEdit,
}: Omit<BrokerTeamMembersListSectionProps, "members" | "memberEditId" | "onToggleMemberStatus" | "onEditMember" | "onDeleteMember">) {
  return (
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
  );
}

export default function BrokerTeamMembersListSection(props: BrokerTeamMembersListSectionProps) {
  const { members, memberEditId, onToggleMemberStatus, onEditMember, onDeleteMember } = props;

  if (!members || members.length === 0) return null;

  return (
    <div className="grid max-h-[55vh] gap-2 overflow-y-auto pr-1">
      {members.map((member, idx) => {
        const memberId = String(member?.member_id || "");
        const infoBits = buildMemberInfoBits(member);
        return (
          <div key={`member-${memberId}-${idx}`} className="grid gap-2">
            <div className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
              <div className="text-[11px] text-white/70">
                <div>
                  <span className="text-white/90">{memberId || "member"}</span>
                  {member?.role ? ` · role ${member.role}` : ""}
                  {member?.status ? ` · ${member.status}` : ""}
                  {member?.agent_id ? ` · agent ${member.agent_id}` : ""}
                  {member?.deployment_id ? ` · dep ${member.deployment_id}` : ""}
                  {member?.created_unix_ms ? ` · ${fmtTs(member.created_unix_ms)}` : ""}
                </div>
                {infoBits.length > 0 ? <div className="text-[10px] text-white/50">{infoBits.join(" · ")}</div> : null}
              </div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => onToggleMemberStatus(member)}
                >
                  {String(member?.status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => onEditMember(member)}
                >
                  Edit
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => onDeleteMember(memberId)}
                >
                  Remove
                </button>
              </div>
            </div>
            {memberEditId === memberId ? <MemberEditPanel {...props} /> : null}
          </div>
        );
      })}
    </div>
  );
}
