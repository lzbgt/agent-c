import React from "react";
import {
  apiBrokerListAgents,
  apiBrokerTeamCreate,
  apiBrokerTeamDelete,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamList,
  type ApiAuth,
  type BrokerAgentInfo,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import type { QuickMember, TeamRow } from "./teamConsoleTypes";

type UseBrokerTeamSetupStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
};

export default function useBrokerTeamSetupState(args: UseBrokerTeamSetupStateArgs) {
  const { base, auth, canQuery } = args;
  const [teamsBusy, setTeamsBusy] = React.useState<boolean>(false);
  const [teamsError, setTeamsError] = React.useState<string | null>(null);
  const [teams, setTeams] = React.useState<TeamRow[] | null>(null);
  const [teamId, setTeamId] = useLocalStorageState<string>("agentui.brokerTeamId", "");
  const [newTeamId, setNewTeamId] = React.useState<string>("");
  const [newTeamName, setNewTeamName] = React.useState<string>("");

  const [quickTeamName, setQuickTeamName] = React.useState<string>("");
  const [quickTeamId, setQuickTeamId] = React.useState<string>("");
  const [quickTeamGoal, setQuickTeamGoal] = React.useState<string>("");
  const [quickBuilderError, setQuickBuilderError] = React.useState<string | null>(null);
  const [quickBuilderBusy, setQuickBuilderBusy] = React.useState<boolean>(false);
  const [quickTemplate, setQuickTemplate] = React.useState<string>("standard");

  const providerDefaults: Record<string, string> = {
    openai: "https://api.openai.com/v1",
    anthropic: "https://api.anthropic.com",
    deepseek: "https://api.deepseek.com",
    moonshot: "https://api.moonshot.cn/v1",
    kimi: "https://api.moonshot.cn/v1",
    glm: "https://open.bigmodel.cn/api/paas/v4",
    local: "",
    custom: "",
  };
  const providerModelDefaults: Record<string, string> = {
    openai: "gpt-4.1",
    anthropic: "claude-3-7-sonnet-20250219",
    deepseek: "deepseek-reasoner",
    moonshot: "kimi-k2.5",
    kimi: "kimi-k2.5",
    glm: "glm-4",
    local: "",
    custom: "",
  };

  const makeQuickMembers = React.useCallback(
    (template: string): QuickMember[] => {
      const mk = (role: string): QuickMember => ({
        id: `${role}-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
        role,
        provider: "openai",
        model: providerModelDefaults.openai,
        baseUrl: providerDefaults.openai,
        agentId: "",
        deploymentId: "",
      });
      if (template === "planner_executor") return [mk("planner"), mk("executor")];
      if (template === "research_team") return [mk("researcher"), mk("executor"), mk("critic")];
      return [mk("planner"), mk("executor"), mk("critic")];
    },
    [],
  );
  const [quickMembers, setQuickMembers] = React.useState<QuickMember[]>(() => makeQuickMembers("standard"));

  const [memberAgentsBusy, setMemberAgentsBusy] = React.useState<boolean>(false);
  const [memberAgentsError, setMemberAgentsError] = React.useState<string | null>(null);
  const [memberAgents, setMemberAgents] = React.useState<BrokerAgentInfo[] | null>(null);

  const teamList = Array.isArray(teams) ? teams : [];
  const teamIdTrimmed = String(teamId || "").trim();

  const refreshTeams = React.useCallback(async () => {
    if (!canQuery) return;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      const resp = await apiBrokerTeamList(base, auth);
      const rows = Array.isArray(resp?.teams) ? resp.teams : [];
      setTeams(rows);
      if (!teamIdTrimmed && rows.length > 0) {
        setTeamId(String(rows[0]?.team_id || ""));
      }
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  }, [auth, base, canQuery, setTeamId, teamIdTrimmed]);

  const refreshMemberAgents = React.useCallback(async () => {
    if (!canQuery) return;
    setMemberAgentsError(null);
    setMemberAgentsBusy(true);
    try {
      const resp = await apiBrokerListAgents(base, auth);
      const rows = Array.isArray(resp?.agents) ? resp.agents : [];
      setMemberAgents(rows);
    } catch (err) {
      setMemberAgentsError(String(err));
    } finally {
      setMemberAgentsBusy(false);
    }
  }, [auth, base, canQuery]);

  const handleCreateTeam = React.useCallback(async () => {
    const tid = String(newTeamId || "").trim();
    if (!tid) {
      setTeamsError("team_id required");
      return;
    }
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamCreate(base, { team_id: tid, display_name: String(newTeamName || "").trim() }, auth);
      setNewTeamId("");
      setNewTeamName("");
      await refreshTeams();
      setTeamId(tid);
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  }, [auth, base, newTeamId, newTeamName, refreshTeams, setTeamId]);

  const handleDeleteTeam = React.useCallback(async () => {
    const tid = teamIdTrimmed;
    if (!tid) return false;
    if (!window.confirm(`Delete team "${tid}"?`)) return false;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamDelete(base, tid, auth);
      setTeamId("");
      await refreshTeams();
      return true;
    } catch (err) {
      setTeamsError(String(err));
      return false;
    } finally {
      setTeamsBusy(false);
    }
  }, [auth, base, refreshTeams, setTeamId, teamIdTrimmed]);

  const slugifyTeamId = React.useCallback((name: string): string => {
    const slug = String(name || "")
      .trim()
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "");
    return slug || `team-${Date.now()}`;
  }, []);

  const handleQuickBuilderApplyTemplate = React.useCallback(
    (template: string) => {
      setQuickTemplate(template);
      setQuickMembers(makeQuickMembers(template));
    },
    [makeQuickMembers],
  );

  const handleQuickMemberUpdate = React.useCallback(
    (id: string, patch: Partial<QuickMember>) => {
      setQuickMembers((prev) =>
        prev.map((member) => {
          if (member.id !== id) return member;
          const next = { ...member, ...patch };
          if (patch.provider !== undefined) {
            const baseDefault = providerDefaults[patch.provider] ?? "";
            const modelDefault = providerModelDefaults[patch.provider] ?? "";
            if (!next.baseUrl || next.baseUrl === providerDefaults[member.provider]) {
              next.baseUrl = baseDefault;
            }
            if (!next.model || next.model === providerModelDefaults[member.provider]) {
              next.model = modelDefault;
            }
          }
          return next;
        }),
      );
    },
    [providerDefaults, providerModelDefaults],
  );

  const handleQuickAddMember = React.useCallback(() => {
    setQuickMembers((prev) => [
      ...prev,
      {
        id: `member-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
        role: "executor",
        provider: "openai",
        model: providerModelDefaults.openai,
        baseUrl: providerDefaults.openai,
        agentId: "",
        deploymentId: "",
      },
    ]);
  }, []);

  const handleQuickRemoveMember = React.useCallback((id: string) => {
    setQuickMembers((prev) => prev.filter((member) => member.id !== id));
  }, []);

  const handleQuickCreateTeam = React.useCallback(
    async (callbacks?: { onCreated?: (teamId: string) => Promise<void> | void }) => {
      if (!canQuery) return false;
      setQuickBuilderError(null);
      setQuickBuilderBusy(true);
      try {
        const name = String(quickTeamName || "").trim();
        if (!name) throw new Error("team name required");
        const id = String(quickTeamId || "").trim() || slugifyTeamId(name);
        const goal = String(quickTeamGoal || "").trim();
        const meta: Record<string, any> = {};
        if (goal) meta.goal = goal;
        const payload: Record<string, any> = { team_id: id, display_name: name };
        if (Object.keys(meta).length > 0) payload.meta = meta;
        const createResp = await apiBrokerTeamCreate(base, payload, auth);
        if (!createResp.ok) throw new Error(createResp.error || createResp.err || createResp.code || "team create failed");

        for (const member of quickMembers) {
          const role = String(member.role || "").trim();
          if (!role) continue;
          const memberPayload: Record<string, any> = { role };
          const agentId = String(member.agentId || "").trim();
          const deploymentId = String(member.deploymentId || "").trim();
          if (agentId) memberPayload.agent_id = agentId;
          if (deploymentId) memberPayload.deployment_id = deploymentId;
          const metaMember: Record<string, any> = {};
          const provider = String(member.provider || "").trim();
          if (provider) metaMember.provider = provider;
          const runOverrides: Record<string, any> = {};
          const model = String(member.model || "").trim();
          if (model) runOverrides.model = model;
          const baseUrl = String(member.baseUrl || "").trim();
          if (baseUrl) runOverrides.base_url = baseUrl;
          if (Object.keys(runOverrides).length > 0) metaMember.run_overrides = runOverrides;
          if (Object.keys(metaMember).length > 0) memberPayload.meta = metaMember;
          const upsertResp = await apiBrokerTeamMembersUpsert(base, id, memberPayload, auth);
          if (!upsertResp.ok) {
            throw new Error(upsertResp.error || upsertResp.err || upsertResp.code || "member create failed");
          }
        }

        setQuickTeamId(id);
        setTeamId(id);
        await refreshTeams();
        if (callbacks?.onCreated) {
          await callbacks.onCreated(id);
        }
        return true;
      } catch (err) {
        setQuickBuilderError(String(err));
        return false;
      } finally {
        setQuickBuilderBusy(false);
      }
    },
    [
      auth,
      base,
      canQuery,
      quickMembers,
      quickTeamGoal,
      quickTeamId,
      quickTeamName,
      refreshTeams,
      setTeamId,
      slugifyTeamId,
    ],
  );

  React.useEffect(() => {
    if (!canQuery) return;
    if (memberAgentsBusy || (memberAgents && memberAgents.length > 0)) return;
    void refreshMemberAgents();
  }, [canQuery, memberAgents, memberAgentsBusy, refreshMemberAgents]);

  return {
    handleCreateTeam,
    handleDeleteTeam,
    handleQuickAddMember,
    handleQuickBuilderApplyTemplate,
    handleQuickCreateTeam,
    handleQuickMemberUpdate,
    handleQuickRemoveMember,
    memberAgents,
    memberAgentsBusy,
    memberAgentsError,
    newTeamId,
    newTeamName,
    providerDefaults,
    providerModelDefaults,
    quickBuilderBusy,
    quickBuilderError,
    quickMembers,
    quickTeamGoal,
    quickTeamId,
    quickTeamName,
    quickTemplate,
    refreshMemberAgents,
    refreshTeams,
    setNewTeamId,
    setNewTeamName,
    setQuickTeamGoal,
    setQuickTeamId,
    setQuickTeamName,
    setTeamId,
    teamId,
    teamIdTrimmed,
    teamList,
    teams,
    teamsBusy,
    teamsError,
  };
}
