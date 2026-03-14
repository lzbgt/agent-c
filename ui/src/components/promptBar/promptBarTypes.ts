import type { ApiAuth } from "../../api";

export type Attachment = {
  path: string;
  name?: string;
  mime?: string;
  kind?: string;
  bytes?: number;
  data_base64?: string;
};

export type PromptBarProps = {
  effectiveBase: string;
  sessionId: string | null | undefined;
  tools: string;
  activeJobId: string | null;
  jobStatus: string | null;
  jobProgressLabel: string;
  runWatchMode: string;
  daemonAuth: ApiAuth;
  prompt: string;
  setPrompt: (next: string) => void;
  runDisabled: boolean;
  runLabel: string;
  queueCount?: number;
  onRun: (vars: { prompt: string; attachments: Attachment[] }) => void;
  setJobNotice: (next: string | null) => void;
  jobNotice: string | null;
  jobError: string | null;
  runError: string | null;
  resultError: string | null;
  clearAttachmentsNonce: number;
  uploadsEnabled?: boolean;
  uploadMaxBytes?: number;
  uploadsDisabledReason?: string;
  chatTarget?: "session" | "team";
  teamId?: string;
  teamAvailable?: boolean;
  onChatTargetChange?: (next: "session" | "team") => void;
  uploadMode?: "session" | "team";
  teamAction?: "run" | "guidance" | "goal";
  onTeamActionChange?: (next: "run" | "guidance" | "goal") => void;
};
