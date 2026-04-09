import type React from "react";
import type { ApiAuth } from "../api";
import type { Attachment } from "../components/PromptBar";

export type TeamActionKind = "run" | "guidance" | "goal";
export type TeamActivityKind = "prompt" | "guidance" | "goal";
export type TeamConversationPayload = Record<string, unknown> | null;

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
  kind?: TeamActivityKind;
  payload?: TeamConversationPayload;
};

export type TeamConversationMemberMeta = {
  role?: string;
  agent_id?: string;
  deployment_id?: string;
};

export type TeamRunSessionSnapshot = {
  sessions: Record<string, string>;
  members: Record<string, TeamConversationMemberMeta>;
  updated_unix_ms: number;
};

export type TeamConversationMessage = {
  role: string;
  content: string;
  created_unix_ms?: number;
  content_truncated?: boolean;
  mm_json?: string;
  mm_bytes?: number;
};

export type TeamConversationMeta = TeamConversationMemberMeta & {
  member_id?: string;
  session_id?: string;
  run_id?: string;
  guidance_id?: string;
  kind?: string;
  priority?: string;
  status?: string;
  payload?: TeamConversationPayload;
};

export type TeamConversationItemKind = "message" | TeamActivityKind;

export type TeamConversationItem = {
  kind: TeamConversationItemKind;
  ts: number;
  message: TeamConversationMessage;
  meta: TeamConversationMeta;
};

export type TeamConversationCache = {
  items: TeamConversationItem[];
  updated_ms: number;
};

export type TeamRecentActivity = {
  key: string;
  label: string;
  preview: string;
  ts: number;
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
