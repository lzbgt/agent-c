import type React from "react";
import type { ApiAuth } from "../api";
import type { Attachment } from "../components/PromptBar";

export type TeamActionKind = "run" | "guidance" | "goal";

export type TeamQueuedAction = {
  prompt: string;
  attachments: Attachment[];
  queued_unix_ms: number;
  action: TeamActionKind;
};

export type TeamRunRequest = {
  prompt: string;
  attachments: Attachment[];
};

export type TeamActivity = {
  ts: number;
  prompt: string;
  run_id?: string;
  kind?: "prompt" | "guidance" | "goal";
  payload?: any;
};

export type TeamRunSessionSnapshot = {
  sessions: Record<string, string>;
  members: Record<string, { role?: string; agent_id?: string; deployment_id?: string }>;
  updated_unix_ms: number;
};

export type TeamConversationCache = {
  items: any[];
  updated_ms: number;
};

export type TeamChatOrchestrationArgs = {
  authKey: string;
  brokerAgentId: string;
  brokerBase: string;
  brokerChatAvailable: boolean;
  connectionMode: string;
  daemonAuth: ApiAuth;
  selectedTeamId: string;
  setAdvancedPage: React.Dispatch<React.SetStateAction<string>>;
  setComposerTaskNonce: React.Dispatch<React.SetStateAction<number>>;
  setJobNotice: React.Dispatch<React.SetStateAction<string | null>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
};
