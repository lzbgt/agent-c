import type { BrokerTeam } from "../../api";

export type TeamRow = BrokerTeam & { owner_sub?: string };

export type QuickMember = {
  id: string;
  role: string;
  provider: string;
  model: string;
  baseUrl: string;
  agentId: string;
  deploymentId: string;
};
