import React from "react";
import FieldLabel from "../FieldLabel";
import BrokerTeamCreatePanel from "./BrokerTeamCreatePanel";
import SectionCard from "./BrokerTeamSectionCard";

type QuickMember = {
  id: string;
  role: string;
  provider: string;
  model: string;
  baseUrl: string;
  agentId: string;
  deploymentId: string;
};

export type BrokerTeamSetupPanelProps = {
  canQuery: boolean;
  teamsBusy: boolean;
  memberAgentsBusy: boolean;
  quickTeamName: string;
  quickTeamId: string;
  quickTeamGoal: string;
  quickTemplate: string;
  quickMembers: QuickMember[];
  quickBuilderBusy: boolean;
  quickBuilderError: string | null;
  providerDefaults: Record<string, string>;
  providerModelDefaults: Record<string, string>;
  memberAgents: Array<Record<string, any>>;
  newTeamId: string;
  newTeamName: string;
  onQuickTeamNameChange: (next: string) => void;
  onQuickTeamIdChange: (next: string) => void;
  onQuickTeamGoalChange: (next: string) => void;
  onQuickMemberUpdate: (id: string, patch: Partial<QuickMember>) => void;
  onQuickAddMember: () => void;
  onQuickRemoveMember: (id: string) => void;
  onQuickCreateTeam: () => void;
  onQuickApplyTemplate: (template: string) => void;
  onRefreshMemberAgents: () => void;
  onNewTeamIdChange: (next: string) => void;
  onNewTeamNameChange: (next: string) => void;
  onCreateTeam: () => void;
};

export default function BrokerTeamSetupPanel(props: BrokerTeamSetupPanelProps) {
  const {
    canQuery,
    teamsBusy,
    memberAgentsBusy,
    quickTeamName,
    quickTeamId,
    quickTeamGoal,
    quickTemplate,
    quickMembers,
    quickBuilderBusy,
    quickBuilderError,
    providerDefaults,
    providerModelDefaults,
    memberAgents,
    newTeamId,
    newTeamName,
    onQuickTeamNameChange,
    onQuickTeamIdChange,
    onQuickTeamGoalChange,
    onQuickMemberUpdate,
    onQuickAddMember,
    onQuickRemoveMember,
    onQuickCreateTeam,
    onQuickApplyTemplate,
    onRefreshMemberAgents,
    onNewTeamIdChange,
    onNewTeamNameChange,
    onCreateTeam,
  } = props;

  return (
    <>
      <SectionCard title="Quick team builder" description="Create a team and add agents in one step." defaultOpen={true}>
        <div className="text-[11px] text-white/60">Defaults stay simple; advanced fields are optional.</div>
        <div className="mt-2 grid gap-2 md:grid-cols-2">
          <div className="grid gap-1">
            <FieldLabel>Team name</FieldLabel>
            <input
              className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={quickTeamName}
              onChange={(e) => onQuickTeamNameChange(e.target.value)}
              placeholder="Ops team"
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Team id</FieldLabel>
            <input
              className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={quickTeamId}
              onChange={(e) => onQuickTeamIdChange(e.target.value)}
              placeholder="auto"
            />
          </div>
          <div className="grid gap-1 md:col-span-2">
            <FieldLabel>Goal</FieldLabel>
            <input
              className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={quickTeamGoal}
              onChange={(e) => onQuickTeamGoalChange(e.target.value)}
              placeholder="What is this team trying to accomplish?"
            />
          </div>
        </div>

        <div className="mt-2 grid gap-2 md:grid-cols-[minmax(0,1fr)_auto_auto]">
          <div className="grid gap-1">
            <FieldLabel>Template</FieldLabel>
            <select
              className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={quickTemplate}
              onChange={(e) => onQuickApplyTemplate(e.target.value)}
            >
              <option value="standard">Planner + Executor + Critic</option>
              <option value="planner_executor">Planner + Executor</option>
              <option value="research_team">Researcher + Executor + Critic</option>
            </select>
          </div>
          <div className="flex items-end">
            <button
              className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={onQuickAddMember}
            >
              Add agent
            </button>
          </div>
          <div className="flex items-end">
            <button
              className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!canQuery || memberAgentsBusy}
              onClick={onRefreshMemberAgents}
            >
              {memberAgentsBusy ? "Refreshing agents…" : "Refresh agents"}
            </button>
          </div>
        </div>

        <div className="grid gap-2">
          <div className="hidden sm:grid sm:grid-cols-2 lg:grid-cols-[minmax(120px,1fr)_minmax(120px,1fr)_minmax(160px,1.3fr)_auto] gap-2 text-[10px] uppercase tracking-wide text-white/40">
            <span>Role</span>
            <span>Provider</span>
            <span>Model</span>
            <span className="lg:text-right">Actions</span>
          </div>
          <div className="grid max-h-[45vh] gap-2 overflow-y-auto pr-1">
            {quickMembers.map((m) => (
              <div key={m.id} className="rounded-md border border-white/10 bg-black/20 p-2">
                <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-[minmax(120px,1fr)_minmax(120px,1fr)_minmax(160px,1.3fr)_auto]">
                  <input
                    aria-label="Role"
                    className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                    value={m.role}
                    onChange={(e) => onQuickMemberUpdate(m.id, { role: e.target.value })}
                    placeholder="role"
                  />
                  <select
                    aria-label="Provider"
                    className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                    value={m.provider}
                    onChange={(e) => onQuickMemberUpdate(m.id, { provider: e.target.value })}
                  >
                    <option value="openai">OpenAI</option>
                    <option value="anthropic">Anthropic</option>
                    <option value="deepseek">DeepSeek</option>
                    <option value="moonshot">Kimi (Moonshot CN)</option>
                    <option value="glm">GLM (Zhipu)</option>
                    <option value="local">Local</option>
                    <option value="custom">Custom</option>
                  </select>
                  <input
                    aria-label="Model"
                    className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                    value={m.model}
                    onChange={(e) => onQuickMemberUpdate(m.id, { model: e.target.value })}
                    placeholder={providerModelDefaults[m.provider] || "model"}
                  />
                  <div className="flex items-center justify-end gap-2 sm:col-span-2 lg:col-span-1">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => onQuickRemoveMember(m.id)}
                      disabled={quickMembers.length <= 1}
                    >
                      Remove
                    </button>
                  </div>
                </div>
                <details className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1">
                  <summary className="cursor-pointer text-[11px] text-white/60">Assignments & overrides</summary>
                  <div className="mt-2 grid gap-2 sm:grid-cols-2">
                    <select
                      aria-label="Agent"
                      className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.agentId}
                      onChange={(e) => onQuickMemberUpdate(m.id, { agentId: e.target.value, deploymentId: "" })}
                    >
                      <option value="">(any agent)</option>
                      {(memberAgents || []).map((agent: any) => (
                        <option key={agent?.agent_id || agent?.id} value={String(agent?.agent_id || agent?.id || "")}>
                          {String(agent?.display_name || agent?.agent_id || agent?.id || "")}
                        </option>
                      ))}
                    </select>
                    <input
                      aria-label="Deployment"
                      className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.deploymentId}
                      onChange={(e) => onQuickMemberUpdate(m.id, { deploymentId: e.target.value })}
                      placeholder="deployment (optional)"
                    />
                  </div>
                  <div className="mt-2 grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto]">
                    <input
                      aria-label="Base URL"
                      className="w-full min-w-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                      value={m.baseUrl}
                      onChange={(e) => onQuickMemberUpdate(m.id, { baseUrl: e.target.value })}
                      placeholder={providerDefaults[m.provider] || "https://api.openai.com/v1"}
                    />
                    <div className="flex items-center">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() =>
                          onQuickMemberUpdate(m.id, {
                            baseUrl: providerDefaults[m.provider] ?? "",
                            model: providerModelDefaults[m.provider] ?? "",
                          })
                        }
                      >
                        Reset defaults
                      </button>
                    </div>
                  </div>
                </details>
              </div>
            ))}
          </div>
        </div>

        {quickBuilderError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
            {quickBuilderError}
          </div>
        ) : null}
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-indigo-500/20 px-3 py-2 text-xs font-semibold text-indigo-100 hover:bg-indigo-500/30 disabled:opacity-50"
            type="button"
            disabled={!canQuery || quickBuilderBusy}
            onClick={onQuickCreateTeam}
          >
            {quickBuilderBusy ? "Creating…" : "Create team + add agents"}
          </button>
          {teamsBusy ? <span className="text-xs text-white/50">Refreshing teams…</span> : null}
        </div>
      </SectionCard>

      <SectionCard title="Create team" description="Manual team creation if you want a minimal team first.">
        <BrokerTeamCreatePanel
          canQuery={canQuery}
          teamsBusy={teamsBusy}
          newTeamId={newTeamId}
          newTeamName={newTeamName}
          onNewTeamIdChange={onNewTeamIdChange}
          onNewTeamNameChange={onNewTeamNameChange}
          onCreateTeam={onCreateTeam}
        />
      </SectionCard>
    </>
  );
}
